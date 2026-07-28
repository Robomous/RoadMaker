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

#pragma once

// The editor's model root: owns the road network, its tessellation, and the
// parser diagnostics. The ONLY mutator of the network — widgets never touch
// the kernel except through this class. QtCore-only; testable offscreen.

#include "roadmaker/edit/command.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "document/scene_sidecar.hpp"

namespace roadmaker::editor {

class Document : public QObject {
  Q_OBJECT

public:
  explicit Document(QObject* parent = nullptr);

  /// Parses `path` and swaps the document in on success (the previous
  /// document is kept on failure, with the error appended to diagnostics).
  /// Emits diagnostics_changed() always; loaded() then mesh_changed() on
  /// success.
  [[nodiscard]] Expected<void> load(const std::filesystem::path& path);

  /// Replaces the document with an empty network (File → New). Mirrors
  /// load(): cancels any preview, clears the undo stack and file path,
  /// then emits loaded(), mesh_changed({}), diagnostics_changed(). Not
  /// undoable — callers prompt on a dirty stack before resetting.
  void reset();

  /// Saves as OpenDRIVE (the .xodr IS the project file, §8). Runs
  /// validate_network first and publishes the findings as the document
  /// diagnostics — checker findings never block the save; only a network
  /// the writer cannot serialize fails. On success records `path`, marks
  /// the undo stack clean, and emits saved().
  [[nodiscard]] Expected<void> save(const std::filesystem::path& path);

  /// Dirty means the undo stack has moved since the last load/save/new.
  [[nodiscard]] bool is_dirty() const { return !undo_stack_.isClean(); }

  /// The scene's Layer-2 state (fmt-s1, #325) as it stands after the last
  /// load/reset — the camera and render mode a reopened scene restores.
  /// Read by ViewportWidget/MainWindow on scene_state_loaded().
  [[nodiscard]] const SceneState& scene_state() const { return scene_state_; }

  /// Installs the live-state provider. `Document` is QtCore-only and cannot
  /// read the viewport, so the app hands it a callback that stamps the CURRENT
  /// camera and render mode onto a state.
  ///
  /// It takes the state BY REFERENCE, seeded with scene_state(), so a provider
  /// overwrites only the fields it owns: a by-value provider returning a fresh
  /// SceneState would arrive with an empty `raw` and silently destroy every
  /// forward-compat key the sidecar was carrying, on the first save.
  void set_scene_state_provider(std::function<void(SceneState&)> provider);

  /// The state a save would write: scene_state() with the provider (if any)
  /// run over it. Both Document::save and AutosaveManager go through this, so
  /// a recovery copy never records a different camera than a real save would.
  [[nodiscard]] SceneState current_scene_state() const;

  /// Re-points a just-loaded recovery copy at the document it recovers
  /// (M3a #53): file_path() becomes the crashed session's original path
  /// (empty when that document was never saved) and the document reads
  /// dirty until the next explicit Save — recovered work must never look
  /// already-saved.
  void mark_recovered(const QString& original_path);

  /// Exports the current tessellation as a binary glTF (.glb).
  [[nodiscard]] Expected<void> export_glb(const std::filesystem::path& path) const;

#ifdef RM_HAVE_USD
  /// Exports the current tessellation as OpenUSD ASCII (.usda). Only present
  /// when the kernel was built with RM_BUILD_USD=ON.
  [[nodiscard]] Expected<void> export_usd(const std::filesystem::path& path) const;
#endif

  [[nodiscard]] const RoadNetwork& network() const { return network_; }

  [[nodiscard]] const NetworkMesh& mesh() const { return mesh_; }

  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

  /// Path of the loaded/saved file; empty until the first successful
  /// load() or save() (and again after reset()).
  [[nodiscard]] QString file_path() const { return file_path_; }

  [[nodiscard]] bool has_file() const { return !file_path_.isEmpty(); }

  /// Editing commands push here (via push_command). Cleared on every
  /// load().
  [[nodiscard]] QUndoStack* undo_stack() { return &undo_stack_; }

  /// The single entry point for kernel mutations
  /// (docs/m2/01_editing_framework.md §1.3): applies the command, and on
  /// success pushes it onto the undo stack (already applied — the stack's
  /// immediate redo is skipped), re-meshes and emits mesh_changed() (and
  /// topology_changed() when the dirty set says so). A failed apply leaves
  /// the document unchanged, appends a diagnostic, and is NOT pushed.
  [[nodiscard]] Expected<void> push_command(std::unique_ptr<edit::Command> command);

  /// The dirty set of the most recently applied command (push_command or a
  /// committed preview). Lets a tool discover ids a command created without a
  /// return channel — e.g. a split's new tail road is the dirty road that is
  /// not the original. Empty before the first mutation.
  [[nodiscard]] const edit::DirtySet& last_dirty() const { return last_dirty_; }

  /// Builds the replacement command for update_preview(). Invoked against
  /// the BASE-state network (the current preview already reverted) so the
  /// command's value snapshots capture pre-session values.
  using PreviewFactory = std::function<std::unique_ptr<edit::Command>(const RoadNetwork&)>;

  // Preview session for drag interactions (docs/design/m2/01_editing_framework.md
  // §3): the network mutates and re-meshes live on every step, but NOTHING
  // enters the undo stack until commit_preview() pushes exactly one entry.
  // push_command() is refused and load() cancels the session while one is
  // active.

  /// Starts a session by applying `command` (re-meshes through the dirty
  /// set). Errors — leaving no session and the network untouched: null
  /// command, a session already active, or a failed apply.
  [[nodiscard]] Expected<void> begin_preview(std::unique_ptr<edit::Command> command);

  /// Replaces the previewed command: reverts the current one, builds the
  /// replacement via `factory` against the restored base state, applies it.
  /// update_preview takes a factory rather than a ready command because
  /// factories snapshot at creation time — a command created while the
  /// previous preview frame was still applied would capture that frame as
  /// its "before" state and undo would restore mid-drag geometry. If the
  /// replacement fails to apply, the previous command is re-applied and the
  /// session stays at its last good state.
  [[nodiscard]] Expected<void> update_preview(const PreviewFactory& factory);

  /// Ends the session pushing the previewed command as the single undo-stack
  /// entry (already applied — no re-apply, no re-mesh). No-op without a
  /// session (a click that never became a drag).
  ///
  /// `already_regenerated` says the previewed command carries its own junction
  /// regeneration (a node drag's move_waypoint_following_junctions does), so
  /// the commit must not regenerate a second time — the second pass would be a
  /// byte-identical no-op, but it would still cost a re-plan and land an extra
  /// child in the undo entry.
  void commit_preview(bool already_regenerated = false);

  /// Ends the session reverting the previewed command: write_xodr() is
  /// byte-identical to the pre-session state. No-op without a session.
  void cancel_preview();

  [[nodiscard]] bool preview_active() const { return preview_command_ != nullptr; }

signals:
  /// Document replaced wholesale — models must reset; entity IDs from before
  /// this signal are stale even when a lookup appears to succeed.
  void loaded();

  /// Tessellation updated. `roads` lists exactly the roads whose meshes
  /// changed — the viewport re-uploads only those. An EMPTY list means
  /// everything changed (load, topology edits, junction-floor updates):
  /// listeners rebuild wholesale.
  void mesh_changed(const std::vector<RoadId>& roads);

  /// Roads or junctions were added or removed by a command (drives tree
  /// resets); fires after mesh_changed().
  void topology_changed();

  /// A road's object/prop layer changed (a prop was placed, moved, or
  /// removed). `roads` lists the owning roads whose prop instances the
  /// viewport should re-upload; an empty list means rebuild wholesale. Prunes
  /// stale prop selections. Fires after the objects re-mesh.
  void objects_changed(const std::vector<RoadId>& roads);

  void diagnostics_changed();

  /// Fired once per push that regenerates a junction — the hairiest lifetime
  /// zone dogfooding found — so AutosaveManager can write a recovery copy
  /// without waiting for a timer tick (#53 gap-fill).
  ///
  /// The GUARANTEE is "an edit that regenerated a junction has a recovery copy",
  /// not a particular instant. The instant differs by edit, which is why this is
  /// no longer called `about_to_regenerate` (cascade-s2, #462):
  ///   - a cross-section edit (lane add/carve/form, road style) regenerates in
  ///     the loop below, and this fires BEFORE that regeneration applies;
  ///   - a MOVE regenerates inside its own atomic command, so by the time the
  ///     push is seen there is no "before" left to capture — this fires just
  ///     after, on a state that is whole either way.
  void regeneration_checkpoint();

  /// A touched junction could not regenerate in place (its turn set changed —
  /// a lane added/removed/retyped on an arm): the edit still lands, but the
  /// junction is left stale until an explicit recreate. MainWindow surfaces
  /// `reason` as a warning toast instead of the old silent log line (finding 2).
  void regeneration_skipped(const QString& reason);

  /// A move could not take a linked neighbour with it, so the link was cut
  /// (cascade-s1, #461). Rare — the kernel re-fits the neighbour whenever it
  /// can — and precisely because it is rare it cannot be predicted at the grab,
  /// which is why this is reported AFTER the edit rather than confirmed before
  /// it. MainWindow surfaces `reason` as a warning toast.
  void links_severed(const QString& reason);

  /// A layer derived from the roads could not follow a move (cascade-s3, #463):
  /// an AUTHORED ground-surface boundary whose roads walked away — never
  /// re-derived, because it is the user's own geometry — or a `<bridge>` span
  /// whose crossing is gone. Surfaces that simply re-derived, and spans that
  /// were relocated onto their crossing, are logged and not surfaced: nothing
  /// went wrong there. MainWindow surfaces `reason` as a warning toast.
  void derived_layer_stale(const QString& reason);

  /// A move drove a prop into a road, a junction floor or another prop
  /// (cascade-s4, #464). Props follow their anchor road's frame, which is
  /// correct and is exactly what can carry one somewhere it does not belong —
  /// most sharply when the road is ROTATED and a prop at large |t| sweeps a
  /// wide arc (#338). Only obstructions this gesture CREATED are reported; one
  /// that was already there is not this move's doing. Nothing is corrected:
  /// Edit > Props > Relocate Obstructed Props is the offered fix, and the user
  /// has to ask for it. MainWindow surfaces `reason` as a warning toast.
  void props_obstructed(const QString& reason);

  /// Written to disk successfully; file_path() points at the file and the
  /// undo stack is clean again.
  void saved();

  /// The scene's Layer-2 state is ready to apply (fmt-s1, #325). Emitted LAST
  /// by load() and reset(), after loaded()/mesh_changed(), because the viewport
  /// arms its post-load auto-framing on loaded() and a restored camera has to
  /// win over it. An absent scene_state().view means "no stored camera" — the
  /// auto-framing then stands, which is what a plain .xodr wants.
  void scene_state_loaded();

private:
  // The undo-stack bridge mutates the network on redo/undo; it is part of
  // Document's own mutation machinery, not an outside caller.
  friend class KernelEditorCommand;

  /// Re-mesh + signals after any kernel mutation (full re-mesh in phase 0;
  /// incremental re-mesh is issue #4).
  void after_kernel_mutation(const edit::DirtySet& dirty);

  /// Pushes an already-applied command, folding regeneration of every
  /// junction it touched (recorded arms) into the same undo entry (02 §6).
  /// `already_meshed` skips the re-mesh when the caller (commit_preview)
  /// already tessellated the primary edit; `already_regenerated` skips the
  /// regeneration pass when the command already did it itself. Shared by
  /// push_command and commit_preview.
  void push_applied_with_regeneration(std::unique_ptr<edit::Command> command,
                                      bool already_meshed,
                                      bool already_regenerated = false);

  /// Best-effort read of the sidecar beside `scene`. Never fails a caller: a
  /// missing file (every plain .xodr) is silent, anything else warns, and both
  /// leave a default-constructed state behind.
  void read_scene_sidecar(const std::filesystem::path& scene);

  RoadNetwork network_;
  NetworkMesh mesh_;
  std::vector<Diagnostic> diagnostics_;
  QString file_path_;
  SceneState scene_state_;
  /// Whether scene_state_ came off disk (rather than being the empty default).
  /// mark_recovered() needs it: a recovery copy with no sidecar must not let
  /// the next save overwrite the ORIGINAL scene's sidecar with defaults.
  bool scene_state_from_disk_ = false;
  std::function<void(SceneState&)> scene_state_provider_;
  QUndoStack undo_stack_;
  edit::DirtySet last_dirty_;
  std::unique_ptr<edit::Command> preview_command_;
};

} // namespace roadmaker::editor
