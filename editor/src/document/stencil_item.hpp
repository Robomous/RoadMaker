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

// The Stencil-library-item → kernel translation (p3-s4), shared by the Marking
// Point tool, the Library drop handler, and the runtime QPainter preview so all
// agree on how a manifest stencil asset maps to edit::StencilParams. The
// crosswalk_item precedent — pure translation + paint, no widgets, unit-testable
// headless (QPixmap works offscreen). Per-instance propagation is deferred to
// p3-s5, but the rm:stencil asset key persisted here is the match key it needs.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/object.hpp"

#include <QPixmap>
#include <QSize>

#include "document/library_manifest.hpp"

namespace roadmaker::editor {

class MaterialCatalog;

/// The edit::StencilParams a Stencil library item authors for a lane of width
/// `lane_width_m`: the glyph subtype + length from the manifest, the width
/// scaled to the lane by `stencil_width_frac`, the paint material/category, and
/// the source asset key. `color` is white paint (road arrows are white).
[[nodiscard]] edit::StencilParams stencil_params_from_item(const LibraryItem& item,
                                                           double lane_width_m,
                                                           const MaterialCatalog& materials);

/// Re-materializes `existing` from `item`'s asset params at lane width
/// `lane_width_m`: rewrites the cornerLocal glyph outline, <material>, and
/// rm:stencil userData while preserving placement (s/t/hdg/z_offset), odr_id,
/// and owning road. Honors material_override (kept for p3-s5's per-instance
/// override path).
[[nodiscard]] Object materialize_stencil(const LibraryItem& item,
                                         const Object& existing,
                                         double lane_width_m,
                                         const MaterialCatalog& materials);

/// A runtime preview pixmap for a Stencil library item (no PNG thumbnail): the
/// arrow glyph filled in the asset's paint colour on a road-surface swatch.
[[nodiscard]] QPixmap render_stencil_preview(const LibraryItem& item,
                                             const QSize& size,
                                             const MaterialCatalog& materials);

} // namespace roadmaker::editor
