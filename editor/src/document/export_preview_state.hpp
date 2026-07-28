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

// The export previews' document layer (p7-s1, #241): everything the two
// preview pages show, computed from the Document and holding no widgets, so it
// is testable headless.
//
// STRICTLY READ-ONLY with respect to the scene. Opening a preview must never
// dirty the document, push a command, or change a byte of what a save would
// write — asserted by OpeningThePreviewNeverDirtiesTheDocument.

#include "roadmaker/io/export_preview.hpp"

namespace roadmaker::editor {

class Document;

/// One snapshot of both previews.
struct ExportPreviewState {
  /// Both mesh formats, always. The USD manifest is computed even in a build
  /// that cannot write .usda — `ScenePreview::available` carries that fact, so
  /// hiding the page would throw away information rather than protect anyone.
  ScenePreview gltf;
  ScenePreview usd;
  XodrPreview xodr;

  /// False until the first recompute — a freshly opened window shows nothing
  /// rather than a manifest of an empty scene.
  bool computed = false;

  /// The document changed since the last recompute. Recomputing is not
  /// automatic: validate_network plus a whole-network prop-obstruction sweep
  /// is too much work to run on every keystroke of a drag.
  bool stale = false;
};

/// Recomputes every page from `document`. Does not mutate it.
void recompute_export_preview(const Document& document, ExportPreviewState& out);

} // namespace roadmaker::editor
