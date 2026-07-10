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
  emit mesh_changed();
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

void Document::after_kernel_mutation(const edit::DirtySet& dirty) {
  mesh_ = build_network_mesh(network_);
  emit mesh_changed();
  if (dirty.topology) {
    emit topology_changed();
  }
}

} // namespace roadmaker::editor
