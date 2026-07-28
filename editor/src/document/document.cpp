/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "document/document.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/io/usd_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/surface_derivation.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
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
  text += "] surfaces=[";
  for (std::size_t i = 0; i < dirty.surfaces.size(); ++i) {
    text += (i == 0 ? "" : ",") + std::to_string(dirty.surfaces[i].index);
  }
  text += "]";
  if (dirty.topology) {
    text += " topology";
  }
  if (dirty.junctions_are_current) {
    text += " junctions_are_current";
  }
  if (dirty.surfaces_are_current) {
    text += " surfaces_are_current";
  }
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

  // Clear the stack (destroying its commands, which may discard against the
  // current network) BEFORE swapping in the loaded one, so no undone command
  // ever discards against a network it never touched (#271). The arena guards
  // would no-op anyway; this makes it safe by construction.
  undo_stack_.clear();
  network_ = std::move(result->network);
  diagnostics_ = std::move(result->diagnostics);
  file_path_ = QString::fromStdString(path.string());
  mesh_ = build_network_mesh(network_);
  // Layer 2 (fmt-s1, #325): comfort state, read AFTER the network is in place
  // and never able to fail the load — the .xodr alone is the scene.
  read_scene_sidecar(path);

  spdlog::info("loaded {} ({} roads, {} diagnostics)",
               path.string(),
               network_.road_count(),
               diagnostics_.size());

  emit loaded();
  emit mesh_changed({});
  emit diagnostics_changed();
  // Last: loaded() arms the viewport's auto-framing, and a restored camera
  // has to overrule it.
  emit scene_state_loaded();
  return {};
}

void Document::read_scene_sidecar(const std::filesystem::path& scene) {
  scene_state_ = SceneState{};
  scene_state_from_disk_ = false;
  const auto sidecar_path = scene_sidecar::path_for(scene);
  auto state = scene_sidecar::load(sidecar_path);
  if (!state) {
    if (state.error().code != ErrorCode::FileNotFound) {
      // Malformed, not missing: worth saying, and self-healed by the next save.
      spdlog::warn("scene sidecar ignored: {} ({})", state.error().message, state.error().context);
    } else {
      // Every pure .xodr lands here — the supported case, not a problem.
      spdlog::debug("no scene sidecar beside {}", scene.string());
    }
    return;
  }
  scene_state_ = std::move(*state);
  scene_state_from_disk_ = true;
  drop_stale_workspace();
}

void Document::drop_stale_workspace() {
  if (!scene_state_.workspace) {
    return;
  }
  // The workspace box is a set of coordinates, and coordinates only mean
  // something in a stated frame (p7-s5, #324). If the .xodr's <geoReference>
  // has changed since the box was framed — someone re-georeferenced the scene,
  // or replaced the file under a sidecar that outlived it — the numbers now
  // describe somewhere else. ADR-0008's rule for a stale sidecar is that it
  // degrades to defaults and never lies about scene content, so the block goes
  // rather than framing the user's workspace on a place that no longer exists.
  const std::string& current = network_.georeference().projection;
  if (scene_state_.workspace->crs == current) {
    return;
  }
  spdlog::warn("scene sidecar: the workspace was framed in a different georeference "
               "('{}', the scene now has '{}') — dropping it",
               scene_state_.workspace->crs,
               current);
  diagnostics_.push_back(Diagnostic{.severity = Severity::Warning,
                                    .location = "header",
                                    .message =
                                        "the saved workspace extents were framed in a different "
                                        "georeference and were discarded",
                                    .rule_id = std::string(rules::kGeoReferenceMismatch)});
  scene_state_.workspace.reset();
}

void Document::set_scene_state_provider(std::function<void(SceneState&)> provider) {
  scene_state_provider_ = std::move(provider);
}

SceneState Document::current_scene_state() const {
  SceneState state = scene_state_;
  if (scene_state_provider_) {
    scene_state_provider_(state);
  }
  return state;
}

void Document::reset() {
  cancel_preview();
  // Clear the stack before replacing the network — see the note in load().
  undo_stack_.clear();
  network_ = RoadNetwork{};
  diagnostics_.clear();
  file_path_.clear();
  scene_state_ = SceneState{};
  scene_state_from_disk_ = false;
  mesh_ = build_network_mesh(network_);

  emit loaded();
  emit mesh_changed({});
  emit diagnostics_changed();
  // Emitted here too (unlike the recovery path): File → New must fall back to
  // the APPLICATION default render mode, not inherit the per-scene override of
  // whatever was open before.
  emit scene_state_loaded();
}

void Document::refresh_diagnostics() {
  // Checker findings replace the document diagnostics — the user sees what a
  // consumer would (§8). Until p7-s1 (#241) this ran ONLY inside save(), so
  // between a load and the next save the panel showed the READER's findings
  // about the file you opened, never the validator's about the network you
  // were building. Nothing here mutates the network.
  diagnostics_ = roadmaker::validate_network(network_);
  emit diagnostics_changed();
}

Expected<void> Document::save(const std::filesystem::path& path) {
  // Never blocks the save — only a network the writer cannot serialize fails.
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
  // Layer 2 beside Layer 0/1 (fmt-s1, #325): a separate file, so the .xodr
  // stays byte-for-byte the pure ASAM export. Written before saved() fires, so
  // every listener (autosave sweep, welcome thumbnail) sees a complete pair.
  // A failure here costs the camera, never the scene — it must not fail a save.
  scene_state_ = current_scene_state();
  if (const auto written_state = scene_sidecar::save(scene_sidecar::path_for(path), scene_state_);
      written_state) {
    scene_state_from_disk_ = true;
  } else {
    spdlog::error("scene sidecar not written: {} ({})",
                  written_state.error().message,
                  written_state.error().context);
  }
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
  // The recovery copy's own sidecar (autosaved beside it) wins — it is the
  // newest state there is. When it is missing, fall back to the ORIGINAL
  // scene's sidecar rather than keeping the empty default: file_path_ is about
  // to point at the user's real file, and the next Save would otherwise
  // overwrite their camera AND every retained forward-compat key with nothing.
  if (!scene_state_from_disk_ && !original_path.isEmpty()) {
    read_scene_sidecar(std::filesystem::path(original_path.toStdString()));
  }
  // No scene_state_loaded() here: the recovered document is already framed, and
  // re-posing the camera under a recovery prompt would be jarring. The state is
  // seated for the next save, which is what was at risk.
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
                                              bool already_meshed,
                                              bool already_regenerated) {
  edit::DirtySet dirty = command->dirty();
  last_dirty_ = dirty; // the primary edit's dirty set (before regenerations)
  spdlog::info("command: {} {}", command->name(), describe_dirty(dirty));

  // A move takes its linked neighbours with it (cascade-s1, #461); when one
  // could not follow, the kernel cut the link and said why. Report it here,
  // AFTER the fact: severing is rare and not knowable at the grab, so the old
  // pre-flight "this will break links" confirmation asked about something that
  // now almost never happens. Read before `command` is moved into the stack.
  for (const edit::FollowRecord& record : command->follow_records()) {
    if (record.outcome != edit::FollowOutcome::Severed) {
      continue;
    }
    const Road* neighbour = network_.road(record.neighbour.road);
    spdlog::warn("link severed: {}", record.reason);
    emit links_severed(tr("%1 could not follow — %2")
                           .arg(neighbour != nullptr
                                    ? tr("Road %1").arg(QString::fromStdString(neighbour->odr_id))
                                    : tr("the neighbour"),
                                QString::fromStdString(record.reason)));
  }

  // And the layers derived from those roads (cascade-s3, #463). Only the two
  // outcomes the user has to know about are surfaced: an authored boundary the
  // move deliberately left alone, and a bridge span with nothing under it. A
  // surface that re-derived and a span that was relocated are the feature
  // working, so they stay in the log.
  for (const edit::DerivedRecord& record : command->derived_records()) {
    if (record.change == edit::DerivedChange::AuthoredBoundaryStale) {
      spdlog::info("authored surface left alone: {}", record.detail);
      emit derived_layer_stale(
          tr("A reshaped ground surface was left where it is — its roads moved"));
    } else if (record.change == edit::DerivedChange::BridgeOrphaned) {
      const Road* road = network_.road(record.road);
      spdlog::warn("bridge span orphaned: {}", record.detail);
      emit derived_layer_stale(
          tr("The bridge on %1 no longer spans anything — %2")
              .arg(road != nullptr ? tr("road %1").arg(QString::fromStdString(road->odr_id))
                                   : tr("a moved road"),
                   QString::fromStdString(record.detail)));
    } else {
      spdlog::info("derived layer recomputed: {}", record.detail);
    }
  }

  // And the props those roads carried (cascade-s4, #464). Every record here is
  // something to surface — unlike the derived layer, there is no "it worked"
  // outcome, because the stage only ever reports obstructions it did not have
  // before. The fix is a menu action, deliberately: a modal opened mid-drag
  // swallows the mouse-release, which is why cascade-s1 removed the last one.
  for (const edit::ObstructionRecord& record : command->obstruction_records()) {
    spdlog::warn("prop obstruction: object {} — {}", record.object_odr_id, record.detail);
    emit props_obstructed(tr("Prop %1 is in the way — %2")
                              .arg(QString::fromStdString(record.object_odr_id),
                                   QString::fromStdString(record.detail)));
  }

  // Editing an incoming road (geometry, elevation, or its lanes) regenerates
  // every junction it touches (02 §6): re-run the generator from each
  // junction's recorded arms, replacing the connecting-road geometry — and,
  // since P2, the turn set — in place. Junctions loaded from foreign files
  // have no recorded arms and are left untouched.
  //
  // Commands that built their own junction structure (create/delete junction,
  // split, delete road) say so with junctions_are_current and are skipped:
  // they list their junction as dirty for re-meshing, but regenerating it
  // would fight the structure they just wrote. That used to key off
  // `topology`, which cannot express "a lane appeared AND the junction needs
  // regenerating" — the case Lane Add and Lane Carve are made of.
  //
  // Every MOVE gesture now says the same thing (cascade-s2, #462): translate,
  // rotate and the four waypoint edits regenerate inside the command, through
  // one funnel, so this loop no longer sees them. What is left here is the
  // edits that change a road's CROSS-SECTION — lane add/remove/carve/form,
  // road style, elevation — which are not previewed and have no funnel of
  // their own.
  std::vector<std::unique_ptr<edit::Command>> regenerations;
  // A command that regenerated its own junctions still owes the recovery copy
  // that this loop's checkpoint provides — it just cannot be taken beforehand,
  // because the regeneration landed atomically with the edit. Taking it here is
  // the same guarantee at a different instant (see the signal's declaration).
  bool announced = dirty.junctions_are_current && !dirty.junctions.empty();
  if (announced) {
    emit regeneration_checkpoint();
  }
  for (const JunctionId junction_id : dirty.junctions) {
    if (dirty.junctions_are_current || already_regenerated) {
      break;
    }
    const Junction* junction = network_.junction(junction_id);
    // A LOCKED junction (#319) opts out of this automatic pass: the user
    // hand-tuned its connections, corners or stop lines and asked for them to
    // survive edits to the arms. regenerate_junction itself never consults the
    // flag, so an explicit "re-derive junction" action still works with no
    // bypass — the lock is a policy of the automatic loops only.
    if (junction == nullptr || junction->arms.empty() || junction->locked) {
      continue;
    }
    if (!announced) {
      // The network holds the primary edit but no regeneration yet — the
      // exact state a recovery copy should capture (#53 gap-fill).
      emit regeneration_checkpoint();
      announced = true;
    }
    auto regen = edit::regenerate_junction(network_, junction_id);
    if (auto applied = regen->apply(network_); !applied.has_value()) {
      // A changed turn set (e.g. a lane added to an arm) cannot regenerate in
      // place — leave the junction for an explicit recreate, don't fail the
      // user's edit. Surface it (finding 2): the user sees a warning toast
      // instead of the junction silently freezing.
      spdlog::warn("junction regeneration skipped: {}", applied.error().message);
      emit regeneration_skipped(QString::fromStdString(applied.error().message));
      continue;
    }
#ifndef NDEBUG
    // The regenerated connecting roads must coincide with their arms (finding 2
    // guard): a breach means the generator and the network drifted apart.
    if (const auto welds = edit::verify_junction_welds(network_, junction_id);
        welds.has_value() && welds->breaches) {
      spdlog::error("junction {} welds breach after regeneration: pos={:.4f} hdg={:.4f}",
                    junction_id.index,
                    welds->max_position_gap,
                    welds->max_heading_gap);
      assert(false && "junction welds breach after regeneration");
    }
#endif
    const edit::DirtySet regen_dirty = regen->dirty();
    for (const RoadId road : regen_dirty.roads) {
      if (std::ranges::find(dirty.roads, road) == dirty.roads.end()) {
        dirty.roads.push_back(road);
      }
    }
    // A regeneration that changed the turn set created or erased connecting
    // roads, so it is topology in its own right — and the primary edit (a lane
    // added, say) never said so. Without this the mesh takes the partial
    // per-road path, which cannot add or drop an item, and prune_stale never
    // runs, leaving a selection pointing at an erased road.
    dirty.topology = dirty.topology || regen_dirty.topology;
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
    // The outgoing preview is reverted and about to be destroyed by the move
    // — release the slots its created objects reserved, or every drag frame
    // with a creator (e.g. Lane Carve previewing split_lane_section) leaks
    // them for the rest of the session (#271).
    preview_command_->discard(network_);
    preview_command_ = std::move(replacement);
    after_kernel_mutation(dirty);
    return {};
  }

  // A failed apply leaves the network untouched (base state): restore the
  // last good preview so the session degrades gracefully mid-drag. The failed
  // replacement may still hold reserved slots — a CompositeCommand unwinds its
  // applied prefix via child reverts — so discard it before it goes out of
  // scope.
  if (replacement != nullptr) {
    replacement->discard(network_);
  }
  if (auto restored = preview_command_->apply(network_); !restored.has_value()) {
    // The original is still reverted and about to be dropped — discard it too.
    spdlog::error("preview restore failed: {}", restored.error().message);
    preview_command_->discard(network_);
    preview_command_.reset();
    return restored;
  }
  return applied;
}

void Document::commit_preview(bool already_regenerated) {
  if (!preview_active()) {
    return;
  }
  // Terrain is deliberately NOT rebuilt during a preview drag (after_kernel_
  // mutation skips it while preview_active), so a road dragged over a terrain
  // field leaves the ground channel stale until release. Capture whether this
  // commit needs it before the command is moved away.
  const edit::DirtySet committed = preview_command_->dirty();
  const bool terrain_needs_rebuild =
      !network_.terrain().empty() &&
      (committed.terrain || committed.topology || !committed.roads.empty());
  // Bridges skip preview frames too (p5-s3): a road dragged over/with a bridge
  // leaves its solids stale until release. Rebuild once if a bridge exists.
  bool bridge_needs_rebuild = !mesh_.bridges.empty();
  if (!bridge_needs_rebuild && (committed.topology || !committed.roads.empty())) {
    network_.for_each_road([&](RoadId, const Road& road) {
      if (!road.bridges.empty()) {
        bridge_needs_rebuild = true;
      }
    });
  }

  // Already applied by begin/update — KernelEditorCommand skips the redo()
  // that QUndoStack fires on push, and the mesh already reflects the state.
  // Fold in any junction regeneration so a dragged arm's junction updates
  // (and undoes) with the drag; the preview already meshed the primary edit.
  push_applied_with_regeneration(
      std::move(preview_command_), /*already_meshed=*/true, already_regenerated);

  // The one thing already_meshed=true does NOT cover: the terrain skipped every
  // preview frame. Rebuild it once, now that the drag has landed, and re-upload
  // the scene wholesale so the ground picks up the road's final position. (A
  // commit that ran junction regeneration already routed through after_kernel_
  // mutation with preview inactive and rebuilt terrain there — this path is the
  // no-regeneration case that skips that hook.)
  if (terrain_needs_rebuild) {
    remesh_terrain(network_, mesh_);
    emit mesh_changed({});
  }
  if (bridge_needs_rebuild) {
    remesh_bridges(network_, mesh_);
    emit mesh_changed({});
  }
}

void Document::cancel_preview() {
  if (!preview_active()) {
    return;
  }
  const edit::DirtySet dirty = preview_command_->dirty();
  spdlog::info("preview cancelled: {} {}", preview_command_->name(), describe_dirty(dirty));
  if (auto reverted = preview_command_->revert(network_); reverted.has_value()) {
    // Reverted cleanly — release the created objects' reserved slots before
    // the command is destroyed (#271). On a failed revert the state is
    // indeterminate, so skip the discard and just drop the command.
    preview_command_->discard(network_);
  } else {
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
  // A prop or signal stores no world pose — its transform is DERIVED from the
  // owning road's frame at mesh time (#400). So a road that moved invalidates
  // every instance it carries even though not one object datum changed:
  // translate/rotate, a node drag's re-fit, an elevation edit, and an erased
  // road (whose instances must go, or they render as ghosts). Deriving it from
  // `roads` here, rather than asking each command to also name `objects`, is the
  // same principle the surfaces below already follow: a derived layer tracks its
  // source's dirt, so no command can forget to say so. Instances only — the
  // markings for these roads were just rebuilt by remesh_roads.
  if (!dirty.roads.empty()) {
    remesh_object_instances(network_, mesh_, dirty.roads);
  }

  // Enclosed-area ground surfaces (#215) follow the roads, driven off the SAME
  // dirty fields — no command sets a surface flag. A topology change can add or
  // remove an enclosed area, so re-derive the surface set and mesh all of them;
  // otherwise the surface SET is unchanged but a bounding road may have moved,
  // so re-mesh only the surfaces touching a changed road. derive_surfaces runs
  // on redo AND undo (both route through this hook) — that is what keeps undo
  // exact: the surface tracks the roads either way.
  // A surface-only edit names its own surfaces: set_surface_material changes no
  // geometry, but set_surface_boundary (p5-s1) reshapes exactly one surface
  // without touching a single road. Re-mesh what the command named and rebuild
  // the scene — the material case pays one cheap redundant fill, which is far
  // cheaper than a second dirty channel that a command could forget to set.
  bool surfaces_changed = !dirty.surfaces.empty();
  if (surfaces_changed) {
    remesh_surfaces(network_, mesh_, dirty.surfaces);
  }
  // A MOVE reconciles the set inside its own command and says so with
  // `surfaces_are_current` (cascade-s3, #463), exactly as it does for junctions.
  // Re-deriving over it would be idempotent and therefore harmless — but it
  // would be a network mutation outside the command layer on the one edit whose
  // undo has to restore a surface's material, and rebuilding the whole channel
  // would mask the scoped `surfaces` list the command already named.
  if (dirty.topology && !dirty.surfaces_are_current) {
    derive_surfaces(network_);
    // remesh_surfaces only rebuilds the SurfaceIds it is handed — an empty span
    // is a no-op, NOT "all" — so gather every surface derive_surfaces left in
    // the arena and rebuild the channel from scratch, dropping any entry whose
    // loop vanished (the cleared channel keeps no stale surface).
    std::vector<SurfaceId> all;
    network_.for_each_surface([&](SurfaceId id, const Surface&) { all.push_back(id); });
    mesh_.surfaces.clear();
    remesh_surfaces(network_, mesh_, all);
    surfaces_changed = true;
  } else if (!dirty.roads.empty()) {
    std::vector<SurfaceId> touched;
    for (const RoadId road : dirty.roads) {
      for (const SurfaceId surface : surfaces_touching(network_, road)) {
        if (std::ranges::find(touched, surface) == touched.end()) {
          touched.push_back(surface);
        }
      }
    }
    if (!touched.empty()) {
      remesh_surfaces(network_, mesh_, touched);
      surfaces_changed = true;
    }
  }

  // Terrain (p5-s2, #232) fills the ground AROUND the roads, so a road edit
  // reshapes it, and a create/remove/undo of the field names it directly. It is
  // ALSO rebuilt whenever a road moved (the footprint it cuts around changed) or
  // topology shifted. Skipped mid-drag: a preview road move must not
  // re-triangulate the whole terrain every frame — commit_preview() runs it
  // once on release (see the note there). derive-then-mesh order does not matter
  // for terrain because it reads the network directly, not the surface channel.
  bool terrain_changed = false;
  const bool has_field = !network_.terrain().empty();
  // A brush stroke (dirty.terrain, p5-s4) is the ONE preview that wants live
  // terrain — the user is sculpting the field itself — so it re-triangulates
  // every frame. A road/junction preview still defers terrain to commit_preview:
  // a per-frame whole-terrain re-mesh would stall the drag, and the road's
  // moving footprint reads fine without the ground until release.
  if ((!preview_active() || dirty.terrain) &&
      (dirty.terrain || (has_field && (dirty.topology || !dirty.roads.empty())))) {
    // dirty.terrain covers create/remove (a remove leaves has_field false but
    // must still clear the stale channel); the has_field guard keeps a plain
    // road edit in a terrain-less scene on the cheap partial-upload path.
    remesh_terrain(network_, mesh_);
    terrain_changed = true;
  }

  // Bridge solids (p5-s3, #233) follow the road geometry: a road or elevation
  // edit re-derives the deck and piers, and authoring/removing a <bridge> rides
  // dirty.roads for its owning road. Skipped mid-drag like terrain (commit_preview
  // runs it once on release). Rebuilt when a road changed AND a bridge exists now
  // or already meshed — so removing the last bridge still clears the channel.
  bool bridges_changed = false;
  if (!preview_active() && (dirty.topology || !dirty.roads.empty())) {
    bool any_bridge = !mesh_.bridges.empty();
    if (!any_bridge) {
      network_.for_each_road([&](RoadId, const Road& road) {
        if (!road.bridges.empty()) {
          any_bridge = true;
        }
      });
    }
    if (any_bridge) {
      remesh_bridges(network_, mesh_);
      bridges_changed = true;
    }
  }

  // Topology, junction-floor, surface, terrain AND bridge changes reshape the
  // item list wholesale; only pure road-geometry edits with nothing else touched
  // ride the partial-upload path. An objects-only edit (roads empty) rebuilds
  // wholesale via the empty list, which now re-reads the prop instances too.
  const bool partial = !dirty.topology && dirty.junctions.empty() && !surfaces_changed &&
                       !terrain_changed && !bridges_changed && !dirty.roads.empty();
  emit mesh_changed(partial ? dirty.roads : std::vector<RoadId>{});
  if (!dirty.objects.empty()) {
    emit objects_changed(dirty.objects); // prunes stale prop selections
  }
  if (dirty.topology) {
    emit topology_changed();
  }
}

} // namespace roadmaker::editor
