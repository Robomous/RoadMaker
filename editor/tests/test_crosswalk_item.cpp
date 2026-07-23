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

// Crosswalk-library-item translation + runtime preview (p3-s2). Verifies the
// manifest→CrosswalkParams mapping, that re-materializing an instance preserves
// its placement and honors a per-instance material override, and that the
// painted preview is non-blank and distinguishes solid from striped.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <QImage>
#include <QPixmap>
#include <vector>

#include "document/crosswalk_item.hpp"
#include "render/material_catalog.hpp"

using roadmaker::editor::LibraryItem;
using roadmaker::editor::MaterialCatalog;

namespace {

LibraryItem zebra_item() {
  LibraryItem item;
  item.key = "crosswalk.zebra";
  item.kind = LibraryItem::Kind::Crosswalk;
  item.crosswalk_width = 3.0;
  item.crosswalk_border = 0.2;
  item.crosswalk_dash = 0.5;
  item.crosswalk_gap = 0.5;
  item.crosswalk_material = "material.paint_white";
  item.crosswalk_segmentation = "crosswalk";
  return item;
}

bool has_bright_pixel(const QImage& image) {
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qGray(image.pixel(x, y)) > 200) {
        return true;
      }
    }
  }
  return false;
}

int bright_pixel_count(const QImage& image) {
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qGray(image.pixel(x, y)) > 200) {
        ++count;
      }
    }
  }
  return count;
}

} // namespace

TEST(CrosswalkItem, ParamsFromItemMapAllFields) {
  const MaterialCatalog materials;
  const roadmaker::edit::CrosswalkParams params =
      roadmaker::editor::crosswalk_params_from_item(zebra_item(), materials);
  EXPECT_DOUBLE_EQ(params.depth_m, 3.0); // manifest "width" = walking depth
  EXPECT_DOUBLE_EQ(params.border_width_m, 0.2);
  EXPECT_DOUBLE_EQ(params.dash_length_m, 0.5);
  EXPECT_DOUBLE_EQ(params.dash_gap_m, 0.5);
  EXPECT_EQ(params.material, "material.paint_white");
  EXPECT_EQ(params.category, "crosswalk");
  EXPECT_EQ(params.asset, "crosswalk.zebra");
}

TEST(CrosswalkItem, MaterializePreservesPlacementAndSpan) {
  const MaterialCatalog materials;
  roadmaker::Object existing;
  existing.odr_id = "cw7";
  existing.type = roadmaker::ObjectType::Crosswalk;
  existing.s = 42.0;
  existing.t = 1.5;
  existing.hdg = 1.5707963;
  existing.z_offset = 0.01;
  existing.length = 7.0; // crossing span across the road

  const roadmaker::Object updated =
      roadmaker::editor::materialize_crosswalk(zebra_item(), existing, materials);
  EXPECT_EQ(updated.odr_id, "cw7");  // identity preserved
  EXPECT_DOUBLE_EQ(updated.s, 42.0); // placement preserved
  EXPECT_DOUBLE_EQ(updated.t, 1.5);
  EXPECT_DOUBLE_EQ(updated.z_offset, 0.01);
  ASSERT_TRUE(updated.length.has_value());
  EXPECT_DOUBLE_EQ(*updated.length, 7.0); // span preserved
  ASSERT_TRUE(updated.width.has_value());
  EXPECT_DOUBLE_EQ(*updated.width, 3.0); // depth from the asset
  ASSERT_TRUE(updated.crosswalk.has_value());
  EXPECT_EQ(updated.crosswalk->material, "material.paint_white");
  ASSERT_EQ(updated.outlines.size(), 1U);
  EXPECT_EQ(updated.outlines.front().corners.size(), 4U);
}

TEST(CrosswalkItem, MaterializeHonorsMaterialOverride) {
  const MaterialCatalog materials;
  roadmaker::Object existing;
  existing.type = roadmaker::ObjectType::Crosswalk;
  existing.s = 10.0;
  existing.length = 5.0;
  existing.crosswalk =
      roadmaker::CrosswalkData{.material = "material.concrete", .material_override = true};

  const roadmaker::Object updated =
      roadmaker::editor::materialize_crosswalk(zebra_item(), existing, materials);
  ASSERT_TRUE(updated.crosswalk.has_value());
  EXPECT_EQ(updated.crosswalk->material, "material.concrete"); // pinned, not the asset's
  EXPECT_TRUE(updated.crosswalk->material_override);
}

TEST(CrosswalkItem, PropagateRewritesFollowingInstancesSkippingOverride) {
  const MaterialCatalog materials;
  roadmaker::RoadNetwork network;
  const std::vector<roadmaker::Waypoint> wp{roadmaker::Waypoint{0.0, 0.0},
                                            roadmaker::Waypoint{120.0, 0.0}};
  const auto road = roadmaker::author_clothoid_road(
      network, wp, roadmaker::LaneProfile::two_lane_default(), "", "1");
  ASSERT_TRUE(road.has_value());

  auto add_crosswalk = [&](const char* id, double s, bool override_material) {
    roadmaker::Object cw;
    cw.odr_id = id;
    cw.type = roadmaker::ObjectType::Crosswalk;
    cw.s = s;
    cw.length = 7.0;
    roadmaker::edit::CrosswalkParams params;
    params.asset = "crosswalk.zebra";
    params.dash_length_m = 0.5;
    params.material = "material.paint_white";
    roadmaker::edit::apply_crosswalk_asset(cw, params);
    if (override_material) {
      cw.crosswalk->material = "material.concrete";
      cw.crosswalk->material_override = true;
    }
    return network.add_object(*road, cw);
  };
  const roadmaker::ObjectId a = add_crosswalk("a", 20.0, false);
  const roadmaker::ObjectId b = add_crosswalk("b", 60.0, true); // pinned material
  // A foreign object (different asset) must be left alone.
  roadmaker::Object other;
  other.odr_id = "tree";
  other.type = roadmaker::ObjectType::Tree;
  other.s = 90.0;
  const roadmaker::ObjectId c = network.add_object(*road, other);

  LibraryItem edited = zebra_item();
  edited.crosswalk_dash = 0.0;                    // asset edit: make it solid
  edited.crosswalk_material = "material.asphalt"; // and change the default material
  auto command =
      roadmaker::editor::propagate_crosswalk_asset(network, edited, materials, "Edit Asset");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  EXPECT_DOUBLE_EQ(network.object(a)->crosswalk->dash_length, 0.0);       // re-materialized
  EXPECT_EQ(network.object(a)->crosswalk->material, "material.asphalt");  // took the new default
  EXPECT_DOUBLE_EQ(network.object(b)->crosswalk->dash_length, 0.0);       // geometry follows
  EXPECT_EQ(network.object(b)->crosswalk->material, "material.concrete"); // material pinned
  EXPECT_EQ(network.object(c)->type, roadmaker::ObjectType::Tree);        // untouched
}

TEST(CrosswalkItem, PropagateReturnsNullWhenNoInstanceFollows) {
  const MaterialCatalog materials;
  roadmaker::RoadNetwork network;
  EXPECT_EQ(roadmaker::editor::propagate_crosswalk_asset(network, zebra_item(), materials, "x"),
            nullptr);
}

TEST(CrosswalkItem, PreviewIsNonBlankAndSolidDiffersFromStriped) {
  const MaterialCatalog materials;
  const QSize size(64, 48);
  const QPixmap striped =
      roadmaker::editor::render_crosswalk_preview(zebra_item(), size, materials);
  ASSERT_FALSE(striped.isNull());
  const QImage striped_img = striped.toImage();
  EXPECT_TRUE(has_bright_pixel(striped_img)); // white stripes present

  LibraryItem solid_item = zebra_item();
  solid_item.crosswalk_dash = 0.0; // solid
  solid_item.crosswalk_border = 0.0;
  const QImage solid_img =
      roadmaker::editor::render_crosswalk_preview(solid_item, size, materials).toImage();
  EXPECT_TRUE(has_bright_pixel(solid_img));
  // A solid fill paints more white than a dashed one of the same size.
  EXPECT_GT(bright_pixel_count(solid_img), bright_pixel_count(striped_img));
}
