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

// The Crosswalk-library-item → kernel translation (p3-s2), shared by the
// generator wiring (context_menu.cpp) and the Attributes-pane asset editor
// (properties_panel.cpp) so both agree on how a manifest crosswalk asset maps
// to edit::CrosswalkParams and how a placed instance re-materializes when its
// asset changes. The runtime QPainter preview lives here too (crosswalk assets
// carry no PNG thumbnail). The marking_item precedent — pure translation +
// paint, no widgets, unit-testable headless (QPixmap works offscreen).

#include "roadmaker/edit/command.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/object.hpp"

#include <QPixmap>
#include <QSize>
#include <memory>
#include <string>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
}

namespace roadmaker::editor {

class MaterialCatalog;

/// The edit::CrosswalkParams a Crosswalk library item authors: stripe geometry,
/// paint material/colour, and the source asset key + segmentation category from
/// the manifest fields. `depth_m` is the manifest `width` (walking depth along
/// the road); the crossing span comes from the placement, not the asset.
[[nodiscard]] edit::CrosswalkParams crosswalk_params_from_item(const LibraryItem& item,
                                                               const MaterialCatalog& materials);

/// Re-materializes `existing` from `item`'s asset params: rewrites the outline,
/// <markings>, and rm:crosswalk userData while preserving placement
/// (s/t/hdg/z_offset), odr_id, owning road, and the crossing span (@length).
/// Honors material_override — an instance whose CrosswalkData pinned its
/// material keeps it when the asset's Default Material changes (GW-5 steps 7/9).
[[nodiscard]] Object materialize_crosswalk(const LibraryItem& item,
                                           const Object& existing,
                                           const MaterialCatalog& materials);

/// A runtime preview pixmap for a Crosswalk library item (no PNG thumbnail):
/// the crossing band with its stripes/solid fill + borders, tinted by the
/// asset's material. `size` is the target size in device-independent pixels.
[[nodiscard]] QPixmap render_crosswalk_preview(const LibraryItem& item,
                                               const QSize& size,
                                               const MaterialCatalog& materials);

/// Builds ONE edit::update_objects command re-materializing every crosswalk
/// instance in `network` whose rm:crosswalk asset key matches `item.key`, from
/// `item`'s current params — the propagation behind an asset edit (GW-5
/// step 7). An instance that pinned its material (material_override) keeps it
/// (materialize_crosswalk honors it). Returns nullptr when no instance follows
/// the asset (nothing to propagate). `name` is the undo-menu label.
[[nodiscard]] std::unique_ptr<edit::Command>
propagate_crosswalk_asset(const RoadNetwork& network,
                          const LibraryItem& item,
                          const MaterialCatalog& materials,
                          const std::string& name);

} // namespace roadmaker::editor
