// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
