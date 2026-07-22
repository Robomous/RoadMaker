// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "document/crosswalk_item.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <algorithm>
#include <utility>
#include <vector>

#include "render/material_catalog.hpp"

namespace roadmaker::editor {

namespace {

/// The paint colour a crosswalk asset's Default Material tints to, for the
/// preview. Resolves the material code through the catalog (its `tint`);
/// unknown codes (e.g. "material.paint_white", which has no PBR texture) fall
/// back to white paint — exactly the crosswalk default.
QColor paint_color(const QString& material_code, const MaterialCatalog& materials) {
  if (!material_code.isEmpty()) {
    if (const MaterialDef* def = materials.find_material(material_code.toStdString())) {
      return QColor::fromRgbF(def->tint[0], def->tint[1], def->tint[2], 1.0F);
    }
  }
  return QColor(Qt::white);
}

} // namespace

edit::CrosswalkParams crosswalk_params_from_item(const LibraryItem& item,
                                                 const MaterialCatalog& /*materials*/) {
  edit::CrosswalkParams params;
  params.depth_m = item.crosswalk_width; // manifest "width" = walking depth
  params.border_width_m = item.crosswalk_border;
  params.dash_length_m = item.crosswalk_dash;
  params.dash_gap_m = item.crosswalk_gap;
  params.material = item.crosswalk_material.toStdString();
  params.category = item.crosswalk_segmentation.toStdString();
  params.asset = item.key.toStdString();
  params.color = "white"; // zebra crossings are white paint
  return params;
}

Object materialize_crosswalk(const LibraryItem& item,
                             const Object& existing,
                             const MaterialCatalog& materials) {
  edit::CrosswalkParams params = crosswalk_params_from_item(item, materials);
  // An instance that pinned its own material keeps it when the asset's Default
  // Material changes (GW-5 steps 7/9).
  const bool pinned = existing.crosswalk.has_value() && existing.crosswalk->material_override;
  if (pinned) {
    params.material = existing.crosswalk->material;
  }
  Object updated = existing; // preserves odr_id, road, s/t/hdg/z_offset, @length (span)
  edit::apply_crosswalk_asset(updated, params);
  if (pinned && updated.crosswalk.has_value()) {
    updated.crosswalk->material_override = true; // apply_crosswalk_asset resets it
  }
  return updated;
}

std::unique_ptr<edit::Command> propagate_crosswalk_asset(const RoadNetwork& network,
                                                         const LibraryItem& item,
                                                         const MaterialCatalog& materials,
                                                         const std::string& name) {
  const std::string key = item.key.toStdString();
  std::vector<std::pair<ObjectId, Object>> updates;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (object.crosswalk.has_value() && object.crosswalk->asset == key) {
      updates.emplace_back(id, materialize_crosswalk(item, object, materials));
    }
  });
  if (updates.empty()) {
    return nullptr; // no instance follows this asset — nothing to propagate
  }
  return edit::update_objects(network, std::move(updates), name);
}

QPixmap render_crosswalk_preview(const LibraryItem& item,
                                 const QSize& size,
                                 const MaterialCatalog& materials) {
  QPixmap pixmap(size);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const double margin = std::max(2.0, size.height() * 0.12);
  const QRectF band(margin, margin, size.width() - (2.0 * margin), size.height() - (2.0 * margin));

  // The road surface the crossing sits on.
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(60, 62, 66));
  painter.drawRoundedRect(band, 3.0, 3.0);

  const QColor paint = paint_color(item.crosswalk_material, materials);
  painter.setBrush(paint);

  const double border =
      item.crosswalk_border > 0.0
          ? std::clamp(item.crosswalk_border / 3.0 * band.height(), 2.0, band.height() * 0.25)
          : 0.0;
  // Border lines frame the crossing along its two road-parallel edges (top and
  // bottom of the band).
  if (border > 0.0) {
    painter.drawRect(QRectF(band.left(), band.top(), band.width(), border));
    painter.drawRect(QRectF(band.left(), band.bottom() - border, band.width(), border));
  }

  const QRectF inner(
      band.left(), band.top() + border, band.width(), band.height() - (2.0 * border));
  if (item.crosswalk_dash <= 0.0) {
    painter.drawRect(inner); // solid crossing
  } else {
    // Stripes repeat across the crossing (bars run along the walking depth).
    const double cycle = item.crosswalk_dash + item.crosswalk_gap;
    const double bars = std::max(1.0, inner.width() / 10.0); // scale to the preview
    const double stripe_frac = item.crosswalk_dash / cycle;
    const double stripe_w = (inner.width() / bars) * stripe_frac;
    const double step = inner.width() / bars;
    for (double x = inner.left(); x < inner.right() - 0.5; x += step) {
      painter.drawRect(QRectF(x, inner.top(), std::max(1.0, stripe_w), inner.height()));
    }
  }
  return pixmap;
}

} // namespace roadmaker::editor
