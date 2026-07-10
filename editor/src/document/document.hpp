#pragma once

// The editor's model root: owns the road network, its tessellation, and the
// parser diagnostics. The ONLY mutator of the network — widgets never touch
// the kernel except through this class. QtCore-only; testable offscreen.

#include "roadmaker/error.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <filesystem>
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

  /// M2 scaffolding: editing commands will push here. Empty in M1, but the
  /// Edit menu binds to it now so undo/redo wiring is structural, not bolted
  /// on later. Cleared on every load().
  [[nodiscard]] QUndoStack* undo_stack() { return &undo_stack_; }

signals:
  /// Document replaced wholesale — models must reset; entity IDs from before
  /// this signal are stale even when a lookup appears to succeed.
  void loaded();

  /// Tessellation rebuilt (fires after loaded(); M2 edits reuse it).
  void mesh_changed();

  void diagnostics_changed();

private:
  RoadNetwork network_;
  NetworkMesh mesh_;
  std::vector<Diagnostic> diagnostics_;
  QString file_path_;
  QUndoStack undo_stack_;
};

} // namespace roadmaker::editor
