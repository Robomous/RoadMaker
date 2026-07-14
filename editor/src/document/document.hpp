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
  void commit_preview();

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

  /// Fired once per push right before junction regeneration applies (the
  /// hairiest lifetime zone dogfooding found) — AutosaveManager writes a
  /// recovery copy of the pre-regeneration state on it (#53 gap-fill).
  void about_to_regenerate();

  /// A touched junction could not regenerate in place (its turn set changed —
  /// a lane added/removed/retyped on an arm): the edit still lands, but the
  /// junction is left stale until an explicit recreate. MainWindow surfaces
  /// `reason` as a warning toast instead of the old silent log line (finding 2).
  void regeneration_skipped(const QString& reason);

  /// Written to disk successfully; file_path() points at the file and the
  /// undo stack is clean again.
  void saved();

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
  /// already tessellated the primary edit. Shared by push_command and
  /// commit_preview.
  void push_applied_with_regeneration(std::unique_ptr<edit::Command> command, bool already_meshed);

  RoadNetwork network_;
  NetworkMesh mesh_;
  std::vector<Diagnostic> diagnostics_;
  QString file_path_;
  QUndoStack undo_stack_;
  edit::DirtySet last_dirty_;
  std::unique_ptr<edit::Command> preview_command_;
};

} // namespace roadmaker::editor
