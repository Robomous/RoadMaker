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
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "document/reference_layers.hpp"
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

  /// Re-runs the OpenDRIVE checker over the CURRENT network and republishes
  /// the findings, emitting diagnostics_changed(). Same semantics as save()'s
  /// validation pass, without writing anything.
  ///
  /// Before p7-s1 (#241) validate_network ran in exactly one place — save() —
  /// so between a load and the next save the diagnostics list described the
  /// file that was opened rather than the network being authored. Never
  /// mutates the network; safe to call from a read-only consumer.
  void refresh_diagnostics();

  /// Appends findings from a one-off operation to the Diagnostics dock
  /// (p7-s4, #244).
  ///
  /// An OSM import produces its compromises during the import itself, not from
  /// re-validating the network afterwards: "way 28374501 was simplified from
  /// 214 nodes to 61" is a fact about a CONVERSION, and `validate_network` —
  /// which only ever sees the result — could not reconstruct it.
  void report_diagnostics(std::vector<Diagnostic> findings);

  /// Dirty means the undo stack has moved since the last load/save/new.
  [[nodiscard]] bool is_dirty() const { return !undo_stack_.isClean(); }

  /// The scene's Layer-2 state (fmt-s1, #325) as it stands after the last
  /// load/reset — the camera and render mode a reopened scene restores.
  /// Read by ViewportWidget/MainWindow on scene_state_loaded().
  [[nodiscard]] const SceneState& scene_state() const { return scene_state_; }

  /// Replaces the Layer-2 state held between saves (p7-s5, #324).
  ///
  /// For the fields NO provider owns — today the workspace box. The camera and
  /// render mode arrive at save time through the provider instead, because
  /// they live in the viewport; the workspace has no such home, so it is held
  /// here and written by the next save.
  ///
  /// Deliberately does NOT mark the document dirty. The workspace is framing,
  /// the same class of state as the camera pose, and moving the camera has
  /// never made a scene need saving either. It rides the next save.
  void set_scene_state(SceneState state) { scene_state_ = std::move(state); }

  /// Imported GIS reference layers (p7-s2, #242).
  ///
  /// Layer-2 state by ADR-0008: it never enters the `.xodr`, and adding or
  /// removing a layer is deliberately NOT undoable — the same ruling the
  /// workspace box already carries. See reference_layers.hpp.
  [[nodiscard]] const ReferenceLayers& reference_layers() const { return reference_layers_; }

  [[nodiscard]] ReferenceLayers& reference_layers() { return reference_layers_; }

  /// The directory reference-layer paths resolve against: the open scene's
  /// folder, or the current directory for an unsaved scene.
  [[nodiscard]] std::filesystem::path scene_directory() const;

  /// Imports `source` as a reference layer and emits reference_layers_changed()
  /// on success. Diagnostics from the read are published; a failure is returned
  /// so the caller can show it, because a refusal naming the CRS is the point.
  Expected<void> add_reference_layer(const std::filesystem::path& source);
  void remove_reference_layer(std::size_t index);
  void set_reference_layer_visible(std::size_t index, bool visible);

  /// Re-derives every layer against the current georeference. Called when the
  /// world georeference changes, so imagery follows the frame rather than
  /// staying where the previous one put it.
  void refit_reference_layers();

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

  // --- the scenario (p8-s2, #246) -------------------------------------------
  //
  // A `.xosc` is a SECOND Layer-0 document, stem-matched beside the `.xodr`
  // (ADR-0014 §9) — not session state, and never folded into the Layer-2
  // sidecar. It lives on `Document` rather than in a sibling class so that
  // stem-matching is automatic, so the ONE-undo-stack invariant stays trivially
  // true, and so SelectionModel (which already holds a `const Document&`) can
  // resolve an actor name with no new wiring.

  [[nodiscard]] const osc::Scenario& scenario() const { return scenario_; }

  /// Whether this scene has a scenario worth writing. An untouched scenario is
  /// NOT saved: an ordinary road project must not sprout an empty `.xosc`
  /// beside every `.xodr`.
  [[nodiscard]] bool has_scenario() const;

  /// The stem-matched `.xosc` path for the currently open scene, or empty when
  /// nothing is open. Derived, never stored — a stored copy could only ever
  /// disagree with the scene it belongs to.
  [[nodiscard]] std::filesystem::path scenario_path() const;

  /// The single entry point for scenario mutations, mirroring push_command:
  /// applies the command and, on success, pushes it onto the SAME QUndoStack
  /// (already applied) and emits scenario_changed(). A failed apply leaves the
  /// scenario unchanged, appends a diagnostic, and is not pushed.
  ///
  /// ★ ONE STACK, INTERLEAVED WITH THE MAP'S. Map and scenario commands share
  /// Document's QUndoStack, which is what makes "switching back to Map mode
  /// returns to it with the undo history intact" (GW-6 step 1) true by
  /// construction rather than by bookkeeping. The kernel's
  /// `osc::edit::ScenarioStack` is Python/headless parity ONLY and must never
  /// drive this document — a document driven by two stacks has no linear
  /// history (docs/design/m2/01_editing_framework.md §1.2).
  [[nodiscard]] Expected<void> push_scenario_command(std::unique_ptr<osc::edit::Command> command);

  [[nodiscard]] const RoadNetwork& network() const { return network_; }

  [[nodiscard]] const NetworkMesh& mesh() const { return mesh_; }

  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

  /// Live CROSS-DOCUMENT findings: `osc::validate_scenario_against_network`
  /// re-run against the CURRENT network whenever the topology or the scenario
  /// changes.
  ///
  /// ★ NAMED FOR THE SCENARIO, NOT FOR ROUTES, since #533. It began as the
  /// route half alone (p8-s3, #247) and now carries every reference whose other
  /// end is in the `.xodr` — signal ids, controller ids, lane anchors and the
  /// upper s-bound as well as routes. That is what makes GW-6 step 8 true in
  /// the editor (delete a lane a route traverses and the route is REPORTED, not
  /// silently dropped or re-routed) AND what closes the hole #533 was filed
  /// for: esmini accepts a dangling signal reference in silence, so this dock
  /// is the only place a user ever learns about one.
  ///
  /// Held apart from diagnostics() on purpose: that vector is a validation
  /// PASS (replaced by refresh_diagnostics/save, appended by command
  /// findings), this one is derived state that must not clobber it and must
  /// not be clobbered by it. DiagnosticsModel presents the two as one table.
  [[nodiscard]] const std::vector<Diagnostic>& scenario_diagnostics() const {
    return scenario_diagnostics_;
  }

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

  /// The reference-layer list or a layer's visibility changed (p7-s2, #242).
  /// The viewport rebuilds its underlay geometry and textures from this.
  void reference_layers_changed();

  /// The scenario changed — an actor was placed, moved, renamed or removed, or
  /// a load replaced the whole document (p8-s2, #246).
  ///
  /// Deliberately NOT mesh_changed(): a scenario holds no arena content and
  /// nothing in it is tessellated, so there is no dirty set and no re-mesh.
  /// Consumers rebuild their actor view from scenario() wholesale — the list is
  /// small, and an incremental protocol would be machinery for a saving nobody
  /// can measure.
  void scenario_changed();

private:
  // The undo-stack bridge mutates the network on redo/undo; it is part of
  // Document's own mutation machinery, not an outside caller.
  friend class KernelEditorCommand;
  // Its scenario twin, for the same reason (p8-s2, #246).
  friend class ScenarioEditorCommand;

  /// Best-effort read of the `.xosc` stem-matched to `scene`. Never fails a
  /// caller: a missing file (every scene without a scenario) is silent, a
  /// malformed one warns and leaves an empty scenario behind. The `.xodr` alone
  /// is the scene — the same contract read_scene_sidecar keeps.
  void read_scenario(const std::filesystem::path& scene);

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

  /// Discards a loaded workspace box that was framed under a different
  /// `<geoReference>` than the scene now carries (p7-s5, #324), with a
  /// structured warning. Called after the sidecar is read and the network is
  /// in place — it needs both.
  void drop_stale_workspace();

  /// Rebuilds the live reference layers from the just-read sidecar, re-reading
  /// every source file against the network's CURRENT georeference. Called from
  /// read_scene_sidecar for the same reason drop_stale_workspace is — it needs
  /// both the sidecar and the network to be in place.
  void restore_reference_layers();

  RoadNetwork network_;
  osc::Scenario scenario_;
  NetworkMesh mesh_;
  std::vector<Diagnostic> diagnostics_;

  /// Recomputes scenario_diagnostics_ from the live network and scenario, and
  /// emits diagnostics_changed() so the panel re-reads the merged table.
  /// Connected (in the constructor) to topology_changed and scenario_changed —
  /// the two events that can invalidate a route. A plain MOVE fires neither
  /// consequence for routes: a waypoint names a lane, not a point in space,
  /// which is GW-6 step 7 holding by construction.
  void refresh_scenario_diagnostics();

  std::vector<Diagnostic> scenario_diagnostics_;
  QString file_path_;
  SceneState scene_state_;
  /// Whether scene_state_ came off disk (rather than being the empty default).
  /// mark_recovered() needs it: a recovery copy with no sidecar must not let
  /// the next save overwrite the ORIGINAL scene's sidecar with defaults.
  bool scene_state_from_disk_ = false;
  std::function<void(SceneState&)> scene_state_provider_;
  ReferenceLayers reference_layers_;
  QUndoStack undo_stack_;
  edit::DirtySet last_dirty_;
  std::unique_ptr<edit::Command> preview_command_;
};

} // namespace roadmaker::editor
