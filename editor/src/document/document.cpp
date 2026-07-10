#include "document/document.hpp"

#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <spdlog/spdlog.h>

#include <utility>

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

} // namespace roadmaker::editor
