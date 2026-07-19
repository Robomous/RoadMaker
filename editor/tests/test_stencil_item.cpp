// Stencil-library-item translation + runtime preview (p3-s4). Verifies the
// manifest→StencilParams mapping (width scales to the lane by width_frac), that
// re-materializing preserves placement, and that the painted glyph preview is
// non-blank and varies by subtype.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <QImage>
#include <QPixmap>

#include "document/stencil_item.hpp"
#include "render/material_catalog.hpp"

using roadmaker::editor::LibraryItem;
using roadmaker::editor::MaterialCatalog;

namespace {

LibraryItem left_arrow_item() {
  LibraryItem item;
  item.key = "stencil.arrow_left";
  item.kind = LibraryItem::Kind::Stencil;
  item.stencil_subtype = "arrowLeft";
  item.stencil_length = 4.0;
  item.stencil_width_frac = 0.5;
  item.stencil_material = "material.paint_white";
  item.stencil_segmentation = "road_marking";
  return item;
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

TEST(StencilItem, ParamsScaleWidthToLane) {
  const MaterialCatalog materials;
  const roadmaker::edit::StencilParams params =
      roadmaker::editor::stencil_params_from_item(left_arrow_item(), 3.5, materials);
  EXPECT_EQ(params.subtype, "arrowLeft");
  EXPECT_DOUBLE_EQ(params.length_m, 4.0);
  EXPECT_DOUBLE_EQ(params.width_m, 3.5 * 0.5); // lane width x width_frac
  EXPECT_EQ(params.material, "material.paint_white");
  EXPECT_EQ(params.category, "road_marking");
  EXPECT_EQ(params.asset, "stencil.arrow_left");
}

TEST(StencilItem, MaterializePreservesPlacementAndKeysAsset) {
  const MaterialCatalog materials;
  roadmaker::Object existing;
  existing.odr_id = "arrow3";
  existing.s = 42.0;
  existing.t = -1.75;
  existing.hdg = 3.14159;
  existing.z_offset = 0.005;

  const roadmaker::Object updated =
      roadmaker::editor::materialize_stencil(left_arrow_item(), existing, 3.5, materials);
  EXPECT_EQ(updated.odr_id, "arrow3");
  EXPECT_DOUBLE_EQ(updated.s, 42.0);
  EXPECT_DOUBLE_EQ(updated.t, -1.75);
  EXPECT_DOUBLE_EQ(updated.hdg, 3.14159); // heading (travel direction) preserved
  EXPECT_EQ(updated.type_str, "roadMark");
  EXPECT_EQ(updated.subtype, "arrowLeft");
  ASSERT_TRUE(updated.stencil.has_value());
  EXPECT_EQ(updated.stencil->asset, "stencil.arrow_left");
  ASSERT_EQ(updated.outlines.size(), 1U);
  EXPECT_FALSE(updated.outlines.front().road_coords); // cornerLocal
}

TEST(StencilItem, MaterializeHonorsMaterialOverride) {
  const MaterialCatalog materials;
  roadmaker::Object existing;
  existing.stencil =
      roadmaker::StencilData{.material = "material.concrete", .material_override = true};
  const roadmaker::Object updated =
      roadmaker::editor::materialize_stencil(left_arrow_item(), existing, 3.5, materials);
  ASSERT_TRUE(updated.stencil.has_value());
  EXPECT_EQ(updated.stencil->material, "material.concrete"); // pinned
  EXPECT_TRUE(updated.stencil->material_override);
}

TEST(StencilItem, PreviewIsNonBlankAndVariesBySubtype) {
  const MaterialCatalog materials;
  const QSize size(64, 48);
  const QImage left =
      roadmaker::editor::render_stencil_preview(left_arrow_item(), size, materials).toImage();
  EXPECT_GT(bright_pixel_count(left), 0); // the glyph is painted

  LibraryItem right = left_arrow_item();
  right.stencil_subtype = "arrowRight";
  const QImage right_img =
      roadmaker::editor::render_stencil_preview(right, size, materials).toImage();
  EXPECT_GT(bright_pixel_count(right_img), 0);
  // Left and right glyphs are mirror images — the painted pixels differ.
  EXPECT_NE(left, right_img);
}
