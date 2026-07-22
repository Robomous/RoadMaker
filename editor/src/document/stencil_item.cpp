// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "document/stencil_item.hpp"

#include "roadmaker/road/object.hpp"

#include <QColor>
#include <QPainter>
#include <QPolygonF>
#include <QRectF>
#include <algorithm>
#include <string>
#include <vector>

#include "render/material_catalog.hpp"

namespace roadmaker::editor {

namespace {

/// The paint colour a stencil asset's Default Material tints to, for the
/// preview — the crosswalk_item precedent (unknown paint codes fall back to
/// white paint).
QColor paint_color(const QString& material_code, const MaterialCatalog& materials) {
  if (!material_code.isEmpty()) {
    if (const MaterialDef* def = materials.find_material(material_code.toStdString())) {
      return QColor::fromRgbF(def->tint[0], def->tint[1], def->tint[2], 1.0F);
    }
  }
  return QColor(Qt::white);
}

} // namespace

edit::StencilParams stencil_params_from_item(const LibraryItem& item,
                                             double lane_width_m,
                                             const MaterialCatalog& /*materials*/) {
  edit::StencilParams params;
  params.subtype = item.stencil_subtype.toStdString();
  params.length_m = item.stencil_length;
  params.width_m = std::max(lane_width_m, 0.1) * item.stencil_width_frac;
  params.material = item.stencil_material.toStdString();
  params.category = item.stencil_segmentation.toStdString();
  params.asset = item.key.toStdString();
  params.color = "white"; // road arrows are white paint
  return params;
}

Object materialize_stencil(const LibraryItem& item,
                           const Object& existing,
                           double lane_width_m,
                           const MaterialCatalog& materials) {
  edit::StencilParams params = stencil_params_from_item(item, lane_width_m, materials);
  const bool pinned = existing.stencil.has_value() && existing.stencil->material_override;
  if (pinned) {
    params.material = existing.stencil->material;
  }
  Object updated = existing; // preserves odr_id, road, s/t/hdg/z_offset
  // apply_stencil_asset never fails for a manifest asset (its subtype is one of
  // the 6 core arrows); on the off chance of a bad manifest, keep the original.
  if (edit::apply_stencil_asset(updated, params).has_value()) {
    if (pinned && updated.stencil.has_value()) {
      updated.stencil->material_override = true; // apply_stencil_asset resets it
    }
    return updated;
  }
  return existing;
}

QPixmap render_stencil_preview(const LibraryItem& item,
                               const QSize& size,
                               const MaterialCatalog& materials) {
  QPixmap pixmap(size);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const double margin = std::max(2.0, size.height() * 0.10);
  const QRectF swatch(
      margin, margin, size.width() - (2.0 * margin), size.height() - (2.0 * margin));
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(60, 62, 66)); // road surface
  painter.drawRoundedRect(swatch, 3.0, 3.0);

  // The glyph outline in the kernel's local (u,v) frame: u along travel, v left.
  const std::vector<roadmaker::OutlineCorner> outline =
      edit::arrow_glyph_outline(item.stencil_subtype.toStdString(), item.stencil_length, 1.75);
  if (outline.size() < 3) {
    return pixmap;
  }
  // Fit the glyph to the swatch, drawn pointing up (u → -screen-y), keeping the
  // aspect ratio.
  double min_u = outline.front().a;
  double max_u = min_u;
  double min_v = outline.front().b;
  double max_v = min_v;
  for (const roadmaker::OutlineCorner& c : outline) {
    min_u = std::min(min_u, c.a);
    max_u = std::max(max_u, c.a);
    min_v = std::min(min_v, c.b);
    max_v = std::max(max_v, c.b);
  }
  const double span_u = std::max(max_u - min_u, 1e-3);
  const double span_v = std::max(max_v - min_v, 1e-3);
  const double scale = std::min(swatch.height() / span_u, swatch.width() / span_v) * 0.9;
  const QPointF centre = swatch.center();
  QPolygonF poly;
  for (const roadmaker::OutlineCorner& c : outline) {
    // v → +x (screen), u → -y (arrow points up); centre the glyph.
    const double x = (c.b - ((min_v + max_v) / 2.0)) * scale;
    const double y = -(c.a - ((min_u + max_u) / 2.0)) * scale;
    poly << QPointF(centre.x() + x, centre.y() + y);
  }
  painter.setBrush(paint_color(item.stencil_material, materials));
  painter.drawPolygon(poly);
  return pixmap;
}

} // namespace roadmaker::editor
