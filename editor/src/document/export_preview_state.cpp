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

#include "document/export_preview_state.hpp"

#include "document/document.hpp"

#include <QFileInfo>

namespace roadmaker::editor {

void recompute_export_preview(const Document& document, ExportPreviewState& out) {
  const NetworkMesh& mesh = document.mesh();
  out.gltf = preview_mesh_export(mesh, MeshExportFormat::Gltf);
  out.usd = preview_mesh_export(mesh, MeshExportFormat::Usd);

  // The document name the writer would stamp, matching Document::save's own
  // rule (the file stem, or "roadmaker" when the scene has never been saved).
  // It matters here beyond cosmetics: it is what names the terrain .asc
  // sidecar, so a preview using a different name would announce a different
  // file than a save would write.
  const QString path = document.file_path();
  const std::string name =
      path.isEmpty() ? std::string{} : QFileInfo(path).completeBaseName().toStdString();
  out.xodr = preview_xodr_export(document.network(), name.empty() ? "roadmaker" : name);

  out.computed = true;
  out.stale = false;
}

} // namespace roadmaker::editor
