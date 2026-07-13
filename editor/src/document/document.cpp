#include "document/document.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/io/usd_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "document/editor_command.hpp"

namespace roadmaker::editor {

namespace {

// The crash-report command trail (#84): every executed command logs its name
// plus the dirty-set parameters, so a report's session log reconstructs what
// the user did. Kernel commands expose no richer parameter view than the
// dirty set — ids are arena indices, stable within a session.
std::string describe_dirty(const edit::DirtySet& dirty) {
  std::string text = "roads=[";
  for (std::size_t i = 0; i < dirty.roads.size(); ++i) {
    text += (i == 0 ? "" : ",") + std::to_string(dirty.roads[i].index);
  }
  text += "] junctions=[";
  for (std::size_t i = 0; i < dirty.junctions.size(); ++i) {
    text += (i == 0 ? "" : ",") + std::to_string(dirty.junctions[i].index);
  }
  text += dirty.topology ? "] topology" : "]";
  return text;
}

} // namespace

Document::Document(QObject* parent) : QObject(parent) {}

Expected<void> Document::load(const std::filesystem::path& path) {
  // Initiating a load ends any drag mid-flight; the revert happens against
  // the network the preview was applied to, before any swap.
  cancel_preview();
  auto result = roadmaker::load_xodr(path);
  if (!result) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .location = result.error().context,
        .message = result.error().message,
    });
    spdlog::error("load failed: {} ({})", result.error().message, result.error().context);
    emit diagnostics_changed();
    return tl::unexpected(result.error());
  }

  network_ = std::move(result->network);
  diagnostics_ = std::move(result->diagnostics);
  file_path_ = QString::fromStdString(path.string());
  undo_stack_.clear();
  mesh_ = build_network_mesh(network_);

  spdlog::info("loaded {} ({} roads, {} diagnostics)",
               path.string(),
               network_.road_count(),
               diagnostics_.size());

  emit loaded();
  emit mesh_changed({});
  emit diagnostics_changed();
  return {};
}

void Document::reset() {
  cancel_preview();
  network_ = RoadNetwork{};
  diagnostics_.clear();
  file_path_.clear();
  undo_stack_.clear();
  mesh_ = build_network_mesh(network_);

  emit loaded();
  emit mesh_changed({});
  emit diagnostics_changed();
}

Expected<void> Document::save(const std::filesystem::path& path) {
  // Checker findings replace the document diagnostics — the user sees what
  // a consumer would (§8) — but never block the save.
  diagnostics_ = roadmaker::validate_network(network_);

  const std::string name = path.stem().string();
  auto written = roadmaker::save_xodr(network_, path, name.empty() ? "roadmaker" : name);
  if (!written) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .location = written.error().context,
        .message = written.error().message,
    });
    spdlog::error("save failed: {} ({})", written.error().message, written.error().context);
    emit diagnostics_changed();
    return written;
  }

  file_path_ = QString::fromStdString(path.string());
  undo_stack_.setClean();
  spdlog::info("saved {} ({} roads, {} diagnostics)",
               path.string(),
               network_.road_count(),
               diagnostics_.size());

  emit saved();
  emit diagnostics_changed();
  return {};
}

void Document::mark_recovered(const QString& original_path) {
  file_path_ = original_path;
  // resetClean (not a moved index) — the loaded recovery stack IS empty,
  // but the content differs from whatever sits at original_path.
  undo_stack_.resetClean();
}

Expected<void> Document::export_glb(const std::filesystem::path& path) const {
  return roadmaker::export_glb(mesh_, path);
}

#ifdef RM_HAVE_USD
Expected<void> Document::export_usd(const std::filesystem::path& path) const {
  return roadmaker::export_usda(mesh_, path);
}
#endif

Expected<void> Document::push_command(std::unique_ptr<edit::Command> command) {
  if (command == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "null command");
  }
  if (preview_active()) {
    return make_error(ErrorCode::InvalidArgument,
                      "push_command during a preview session (commit or cancel first)");
  }
  const std::string name(command->name());
  if (auto applied = command->apply(network_); !applied.has_value()) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .location = applied.error().context,
        .message = name + ": " + applied.error().message,
    });
    spdlog::error("{} failed: {}", name, applied.error().message);
    emit diagnostics_changed();
    return applied;
  }
  // Already applied above — KernelEditorCommand skips the redo() that
  // QUndoStack fires on push.
  push_applied_with_regeneration(std::move(command), /*already_meshed=*/false);
  return {};
}

void Document::push_applied_with_regeneration(std::unique_ptr<edit::Command> command,
                                              bool already_meshed) {
  edit::DirtySet dirty = command->dirty();
  last_dirty_ = dirty; // the primary edit's dirty set (before regenerations)
  spdlog::info("command: {} {}", command->name(), describe_dirty(dirty));

  // Editing an incoming road (geometry, elevation) regenerates every junction
  // it touches (02 §6): re-run the generator from each junction's recorded
  // arms, replacing the connecting-road geometry in place. Junctions loaded
  // from foreign files have no recorded arms and are left untouched.
  // Topology commands (create/delete junction, split, delete road) are
  // skipped: they list their own junction as dirty but must not self- or
  // double-regenerate — the create already built the connecting roads.
  std::vector<std::unique_ptr<edit::Command>> regenerations;
  bool announced = false;
  for (const JunctionId junction_id : dirty.junctions) {
    if (dirty.topology) {
      break;
    }
    const Junction* junction = network_.junction(junction_id);
    if (junction == nullptr || junction->arms.empty()) {
      continue;
    }
    if (!announced) {
      // The network holds the primary edit but no regeneration yet — the
      // exact state a recovery copy should capture (#53 gap-fill).
      emit about_to_regenerate();
      announced = true;
    }
    auto regen = edit::regenerate_junction(network_, junction_id);
    if (auto applied = regen->apply(network_); !applied.has_value()) {
      // A changed turn set (e.g. a lane added to an arm) cannot regenerate in
      // place — leave the junction for an explicit recreate, don't fail the
      // user's edit.
      spdlog::warn("junction regeneration skipped: {}", applied.error().message);
      continue;
    }
    const edit::DirtySet regen_dirty = regen->dirty();
    for (const RoadId road : regen_dirty.roads) {
      if (std::ranges::find(dirty.roads, road) == dirty.roads.end()) {
        dirty.roads.push_back(road);
      }
    }
    regenerations.push_back(std::move(regen));
  }

  if (regenerations.empty()) {
    undo_stack_.push(new KernelEditorCommand(*this, std::move(command)));
    if (!already_meshed) {
      after_kernel_mutation(dirty);
    }
    return;
  }
  // Group the edit and its regenerations into ONE undo entry so a single
  // Ctrl+Z reverts both together (all already applied — the wrappers skip
  // their first redo).
  undo_stack_.beginMacro(
      QString::fromUtf8(command->name().data(), static_cast<qsizetype>(command->name().size())));
  undo_stack_.push(new KernelEditorCommand(*this, std::move(command)));
  for (auto& regen : regenerations) {
    undo_stack_.push(new KernelEditorCommand(*this, std::move(regen)));
  }
  undo_stack_.endMacro();
  after_kernel_mutation(dirty);
}

Expected<void> Document::begin_preview(std::unique_ptr<edit::Command> command) {
  if (command == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "null command");
  }
  if (preview_active()) {
    return make_error(ErrorCode::InvalidArgument, "a preview session is already active");
  }
  if (auto applied = command->apply(network_); !applied.has_value()) {
    return applied;
  }
  preview_command_ = std::move(command);
  after_kernel_mutation(preview_command_->dirty());
  return {};
}

Expected<void> Document::update_preview(const PreviewFactory& factory) {
  if (!preview_active()) {
    return make_error(ErrorCode::InvalidArgument, "no preview session active");
  }
  if (!factory) {
    return make_error(ErrorCode::InvalidArgument, "null preview factory");
  }
  // Back to the base state so the replacement snapshots pre-session values.
  if (auto reverted = preview_command_->revert(network_); !reverted.has_value()) {
    // An applied command must revert (kernel contract); treat a violation as
    // a broken session rather than mutating further.
    spdlog::error("preview revert failed: {}", reverted.error().message);
    preview_command_.reset();
    return reverted;
  }

  std::unique_ptr<edit::Command> replacement = factory(network_);
  Expected<void> applied = replacement != nullptr
                               ? replacement->apply(network_)
                               : Expected<void>(make_error(ErrorCode::InvalidArgument,
                                                           "preview factory returned no command"));
  if (applied.has_value()) {
    const edit::DirtySet dirty = replacement->dirty();
    preview_command_ = std::move(replacement);
    after_kernel_mutation(dirty);
    return {};
  }

  // A failed apply leaves the network untouched (base state): restore the
  // last good preview so the session degrades gracefully mid-drag.
  if (auto restored = preview_command_->apply(network_); !restored.has_value()) {
    spdlog::error("preview restore failed: {}", restored.error().message);
    preview_command_.reset();
    return restored;
  }
  return applied;
}

void Document::commit_preview() {
  if (!preview_active()) {
    return;
  }
  // Already applied by begin/update — KernelEditorCommand skips the redo()
  // that QUndoStack fires on push, and the mesh already reflects the state.
  // Fold in any junction regeneration so a dragged arm's junction updates
  // (and undoes) with the drag; the preview already meshed the primary edit.
  push_applied_with_regeneration(std::move(preview_command_), /*already_meshed=*/true);
}

void Document::cancel_preview() {
  if (!preview_active()) {
    return;
  }
  const edit::DirtySet dirty = preview_command_->dirty();
  spdlog::info("preview cancelled: {} {}", preview_command_->name(), describe_dirty(dirty));
  if (auto reverted = preview_command_->revert(network_); !reverted.has_value()) {
    spdlog::error("preview cancel failed to revert: {}", reverted.error().message);
  }
  preview_command_.reset();
  after_kernel_mutation(dirty);
}

void Document::after_kernel_mutation(const edit::DirtySet& dirty) {
  remesh_roads(network_, mesh_, dirty.roads);
  remesh_junctions(network_, mesh_, dirty.junctions);
  // Prop layer: regenerate only the owning roads' instances (no road-surface
  // re-tessellation) via the reserved objects channel.
  if (!dirty.objects.empty()) {
    remesh_objects(network_, mesh_, dirty.objects);
  }
  // Topology and junction-floor changes reshape the item list wholesale;
  // only pure road-geometry edits ride the partial-upload path. An
  // objects-only edit (roads empty) rebuilds wholesale via the empty list,
  // which now re-reads the prop instances too.
  const bool partial = !dirty.topology && dirty.junctions.empty() && !dirty.roads.empty();
  emit mesh_changed(partial ? dirty.roads : std::vector<RoadId>{});
  if (!dirty.objects.empty()) {
    emit objects_changed(dirty.objects); // prunes stale prop selections
  }
  if (dirty.topology) {
    emit topology_changed();
  }
}

} // namespace roadmaker::editor
