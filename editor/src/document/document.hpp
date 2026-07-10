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

  /// Exports the current tessellation as a binary glTF (.glb).
  [[nodiscard]] Expected<void> export_glb(const std::filesystem::path& path) const;

  [[nodiscard]] const RoadNetwork& network() const { return network_; }

  [[nodiscard]] const NetworkMesh& mesh() const { return mesh_; }

  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

  /// Path of the loaded file; empty until the first successful load.
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

  void diagnostics_changed();

private:
  // The undo-stack bridge mutates the network on redo/undo; it is part of
  // Document's own mutation machinery, not an outside caller.
  friend class KernelEditorCommand;

  /// Re-mesh + signals after any kernel mutation (full re-mesh in phase 0;
  /// incremental re-mesh is issue #4).
  void after_kernel_mutation(const edit::DirtySet& dirty);

  RoadNetwork network_;
  NetworkMesh mesh_;
  std::vector<Diagnostic> diagnostics_;
  QString file_path_;
  QUndoStack undo_stack_;
};

} // namespace roadmaker::editor
