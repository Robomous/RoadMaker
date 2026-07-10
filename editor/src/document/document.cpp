#include "document/document.hpp"

#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <spdlog/spdlog.h>

#include <utility>

#include "document/editor_command.hpp"

namespace roadmaker::editor {

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

Expected<void> Document::export_glb(const std::filesystem::path& path) const {
  return roadmaker::export_glb(mesh_, path);
}

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
  const edit::DirtySet dirty = command->dirty();
  // Already applied above — KernelEditorCommand skips the redo() that
  // QUndoStack fires on push.
  undo_stack_.push(new KernelEditorCommand(*this, std::move(command)));
  after_kernel_mutation(dirty);
  return {};
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
  undo_stack_.push(new KernelEditorCommand(*this, std::move(preview_command_)));
}

void Document::cancel_preview() {
  if (!preview_active()) {
    return;
  }
  const edit::DirtySet dirty = preview_command_->dirty();
  if (auto reverted = preview_command_->revert(network_); !reverted.has_value()) {
    spdlog::error("preview cancel failed to revert: {}", reverted.error().message);
  }
  preview_command_.reset();
  after_kernel_mutation(dirty);
}

void Document::after_kernel_mutation(const edit::DirtySet& dirty) {
  remesh_roads(network_, mesh_, dirty.roads);
  remesh_junctions(network_, mesh_, dirty.junctions);
  // Topology and junction-floor changes reshape the item list wholesale;
  // only pure road-geometry edits ride the partial-upload path.
  const bool partial = !dirty.topology && dirty.junctions.empty() && !dirty.roads.empty();
  emit mesh_changed(partial ? dirty.roads : std::vector<RoadId>{});
  if (dirty.topology) {
    emit topology_changed();
  }
}

} // namespace roadmaker::editor
