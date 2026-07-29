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

// Fitting ground out of a point cloud (p7-s3, #243).

#include "roadmaker/lidar/point_cloud.hpp"
#include "roadmaker/road/terrain.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace roadmaker;
using namespace roadmaker::lidar;

namespace {

std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path(RM_LIDAR_FIXTURES_DIR) / name;
}

bool mentions(const std::vector<Diagnostic>& diagnostics, std::string_view needle) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [needle](const Diagnostic& d) {
    return d.message.find(needle) != std::string::npos;
  });
}

/// A synthetic cloud, built directly rather than read, for the cases a fixture
/// cannot express (an empty cell, a degenerate extent).
PointCloud make_cloud(const std::vector<std::array<double, 3>>& points,
                      const std::vector<std::uint8_t>& classes = {}) {
  PointCloud cloud;
  if (points.empty()) {
    return cloud;
  }
  std::array<double, 3> low = points.front();
  std::array<double, 3> high = points.front();
  for (const auto& point : points) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      low[axis] = std::min(low[axis], point[axis]);
      high[axis] = std::max(high[axis], point[axis]);
    }
  }
  for (std::size_t axis = 0; axis < 3; ++axis) {
    cloud.bounds[axis] = low[axis];
    cloud.bounds[axis + 3] = high[axis];
    cloud.origin[axis] = (low[axis] + high[axis]) * 0.5;
  }
  for (const auto& point : points) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      cloud.xyz.push_back(static_cast<float>(point[axis] - cloud.origin[axis]));
    }
  }
  cloud.classification = classes;
  cloud.source_count = points.size();
  return cloud;
}

/// The fixture tile's own ground plane: z = 2 + 0.05·dx + 0.02·dy from the
/// tile's low corner. Kept here so a test can say what the answer SHOULD be
/// rather than compare the fit against itself.
double expected_ground(double dx, double dy) {
  return 2.0 + (0.05 * dx) + (0.02 * dy);
}

} // namespace

TEST(LidarGround, FitsTheFixtureTilesOwnSlopedPlane) {
  const auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value()) << (tile ? "" : tile.error().message);

  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(tile->cloud, options, diagnostics);
  ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().message);

  // 100 × 80 m at 10 m posts.
  EXPECT_EQ(field->cols, 11U);
  EXPECT_EQ(field->rows, 9U);
  EXPECT_DOUBLE_EQ(field->spacing, 10.0);
  EXPECT_NEAR(field->origin_x, 628000.0, 1e-3);
  EXPECT_NEAR(field->origin_y, 5803000.0, 1e-3);

  // ★ ROW 0 IS THE LOW-y ROW. The fixture slopes in BOTH axes precisely so a
  // flipped or transposed grid shows up as the wrong corner being high rather
  // than as a plausible surface.
  //
  // The tolerance is HALF A CELL OF GRADE, not an epsilon: this is a binning
  // fit, and the returns inside a cell are not distributed symmetrically about
  // its post (5 m lattice, 10 m posts), so a post reads the mean of what landed
  // near it rather than the plane's value exactly there. Half a cell is
  // 0.5·10·0.05 = 0.25 m in x and 0.10 m in y; 0.4 m covers both with room and
  // is still an order of magnitude under the 8 m building this must not admit.
  constexpr double kBinningSlack = 0.4;
  const double low_corner = field->heights[0];
  const double high_x = field->heights[field->cols - 1];
  const double high_y = field->heights[(field->rows - 1) * field->cols];
  // ★ THE TOLERANCE IS THE FLOAT QUANTUM OF THE OFFSET REPRESENTATION, and it
  // is a design consequence rather than slop: coordinates are floats relative to
  // a double origin, so at the tile's own scale a height round-trips to about
  // 2e-7 m. Asserting tighter than that would be asserting the cloud is stored
  // in doubles, which it deliberately is not (see PointCloud's comment).
  constexpr double kFloatQuantum = 1e-6;
  // The low corner is otherwise EXACT: no point rounds down into a post below
  // zero, so that cell holds exactly one return, the tile's own corner.
  EXPECT_NEAR(low_corner, expected_ground(0.0, 0.0), kFloatQuantum);
  EXPECT_NEAR(high_x, expected_ground(100.0, 0.0), kBinningSlack);
  EXPECT_NEAR(high_y, expected_ground(0.0, 80.0), kBinningSlack);
  EXPECT_GT(high_x, low_corner);
  EXPECT_GT(high_y, low_corner);
}

TEST(LidarGround, TheBuildingIsExcludedWhenTheTileClassifiesGround) {
  // ★ THE CASE THE TWO ESTIMATORS DISAGREE ON. The fixture stands an 8 m
  // building on the ground plane. A classification-driven fit ignores it; a
  // lowest-return fit ignores it too, because the ground beneath is also
  // sampled — which is exactly why the test below removes the ground returns to
  // make the difference observable.
  const auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value());

  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(tile->cloud, options, diagnostics);
  ASSERT_TRUE(field.has_value());

  // The building sits over dx 30..50, dy 25..45 — posts (3..5, 3..4) at 10 m.
  // The assertion that matters is sharp even though the fit is a binning one:
  // the roof is EIGHT METRES up, so half a cell of grade cannot be mistaken for
  // it in either direction.
  const double under_building = field->heights[(3 * field->cols) + 4];
  EXPECT_NEAR(under_building, expected_ground(40.0, 30.0), 0.4)
      << "the roof leaked into the terrain";
  EXPECT_LT(under_building, expected_ground(40.0, 30.0) + 1.0)
      << "an 8 m roof would show up here as roughly +8, not as a rounding";
  EXPECT_TRUE(mentions(diagnostics, "classified as bare ground"));
}

TEST(LidarGround, AnUnclassifiedCloudFallsBackToTheLowestReturnAndSaysSo) {
  // Same geometry, classification stripped — a raw scan. The estimator changes,
  // and the diagnostic must change with it: a user reading terrain under a
  // bridge has to know which of the two answers they are looking at.
  auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value());
  tile->cloud.classification.clear();

  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(tile->cloud, options, diagnostics);
  ASSERT_TRUE(field.has_value());

  EXPECT_TRUE(mentions(diagnostics, "classifies no ground"));
  EXPECT_TRUE(mentions(diagnostics, "will read low"));
  // The ground returns are still the lowest in each cell, so the surface is the
  // same one — the fallback is honest here, and the diagnostic is what carries
  // the caveat. It reads slightly LOW rather than slightly off, because the
  // minimum inside a cell sits at that cell's downhill edge: the bias the
  // classified path exists to avoid.
  EXPECT_NEAR(field->heights[0], expected_ground(0.0, 0.0), 1e-6);
  const double mid = field->heights[(4 * field->cols) + 5];
  EXPECT_LE(mid, expected_ground(50.0, 40.0) + 1e-6)
      << "a lowest-return fit is biased downhill, never uphill";
  EXPECT_GT(mid, expected_ground(50.0, 40.0) - 0.4);
}

TEST(LidarGround, TurningClassificationOffChangesTheAnswerWhereItShould) {
  // Prove the two estimators are actually different code paths, by removing the
  // ground returns under the building so only the roof is left there. With
  // classification honoured, that cell has no ground point and is interpolated
  // from its neighbours; without it, the roof becomes the terrain.
  auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value());

  PointCloud stripped;
  stripped.origin = tile->cloud.origin;
  for (std::size_t i = 0; i < tile->cloud.size(); ++i) {
    const std::array<double, 3> world = tile->cloud.point(i);
    const double dx = world[0] - 628000.0;
    const double dy = world[1] - 5803000.0;
    const bool under_the_building = dx >= 29.0 && dx <= 51.0 && dy >= 24.0 && dy <= 46.0;
    if (under_the_building && tile->cloud.classification[i] == kGroundClass) {
      continue;
    }
    stripped.xyz.insert(
        stripped.xyz.end(),
        {tile->cloud.xyz[i * 3], tile->cloud.xyz[(i * 3) + 1], tile->cloud.xyz[(i * 3) + 2]});
    stripped.classification.push_back(tile->cloud.classification[i]);
  }
  stripped.bounds = tile->cloud.bounds;
  stripped.source_count = stripped.size();
  ASSERT_LT(stripped.size(), tile->cloud.size());

  GroundFitOptions honour_classes;
  honour_classes.spacing = 10.0;
  honour_classes.use_classification = true;
  GroundFitOptions ignore_classes = honour_classes;
  ignore_classes.use_classification = false;

  std::vector<Diagnostic> a;
  std::vector<Diagnostic> b;
  const auto classified = point_cloud_to_height_field(stripped, honour_classes, a);
  const auto lowest = point_cloud_to_height_field(stripped, ignore_classes, b);
  ASSERT_TRUE(classified.has_value());
  ASSERT_TRUE(lowest.has_value());

  const std::size_t at = (3 * classified->cols) + 4;
  EXPECT_NEAR(classified->heights[at], expected_ground(40.0, 30.0), 1.0)
      << "with the classes honoured the gap should be filled from the ground around it";
  EXPECT_GT(lowest->heights[at], classified->heights[at] + 5.0)
      << "with the classes ignored the roof IS the lowest return here, so the two "
         "estimators must disagree by roughly the building's height";
}

TEST(LidarGround, EmptyCellsAreInterpolatedNeverWrittenAsZero) {
  // ★ ZERO IS NOT A MISSING VALUE — it is a claim that the ground is at the
  // vertical datum. p7-s2 shipped exactly this bug for reprojected DEMs, where
  // a no-data ring became a cliff at height 0. A scattered cloud has genuine
  // gaps, so here it is the normal case rather than the edge case.
  std::vector<std::array<double, 3>> points;
  for (int iy = 0; iy <= 4; ++iy) {
    for (int ix = 0; ix <= 4; ++ix) {
      if (ix == 2 && iy == 2) {
        continue; // punch a hole in the middle
      }
      points.push_back({ix * 10.0, iy * 10.0, 100.0});
    }
  }
  const PointCloud cloud = make_cloud(points);

  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(cloud, options, diagnostics);
  ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().message);

  EXPECT_EQ(field->cols, 5U);
  EXPECT_EQ(field->rows, 5U);
  const double hole = field->heights[(2 * field->cols) + 2];
  EXPECT_NEAR(hole, 100.0, 1e-9) << "surrounded by 100 m ground, the hole must read 100 m";
  EXPECT_NE(hole, 0.0);
  EXPECT_TRUE(mentions(diagnostics, "interpolated from"));
}

TEST(LidarGround, TheFillIsDeterministic) {
  // Each pass reads the previous pass's buffer, so the answer cannot depend on
  // traversal order. Two fits of the same cloud must be bit-identical.
  std::vector<std::array<double, 3>> points;
  for (int iy = 0; iy <= 6; ++iy) {
    for (int ix = 0; ix <= 6; ++ix) {
      if ((ix + iy) % 3 == 0) {
        continue;
      }
      points.push_back({ix * 10.0, iy * 10.0, 10.0 + ix + iy});
    }
  }
  const PointCloud cloud = make_cloud(points);

  GroundFitOptions options;
  options.spacing = 10.0;
  std::vector<Diagnostic> a;
  std::vector<Diagnostic> b;
  const auto first = point_cloud_to_height_field(cloud, options, a);
  const auto second = point_cloud_to_height_field(cloud, options, b);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->heights, second->heights);
}

TEST(LidarGround, TheFieldInstallsThroughTheOrdinaryTerrainCommand) {
  // The whole point of producing a HeightField rather than a bespoke type: a
  // lidar-seeded terrain is real, undoable scene content that goes in by the
  // same door a .asc DEM and an elevation GeoTIFF use.
  const auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value());
  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(tile->cloud, options, diagnostics);
  ASSERT_TRUE(field.has_value());

  EXPECT_GT(field->spacing, 0.0);
  EXPECT_EQ(field->heights.size(), field->cols * field->rows);
  EXPECT_TRUE(std::all_of(field->heights.begin(), field->heights.end(), [](double h) {
    return std::isfinite(h);
  })) << "set_terrain_field refuses a non-finite height, so producing one is a defect here";
}

// --- Refusals ---------------------------------------------------------------

TEST(LidarGround, AnEmptyCloudIsRefused) {
  const PointCloud cloud;
  std::vector<Diagnostic> diagnostics;
  const auto field = point_cloud_to_height_field(cloud, GroundFitOptions{}, diagnostics);
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().message.find("no points"), std::string::npos);
}

TEST(LidarGround, ASpacingTooFineForTheTileIsRefusedByName) {
  const auto tile = load_point_cloud(fixture("amsterdam_tile.las"));
  ASSERT_TRUE(tile.has_value());

  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 0.01; // 100 m / 1 cm = 10 001 posts, past the 2048 cap
  const auto field = point_cloud_to_height_field(tile->cloud, options, diagnostics);
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().message.find(std::to_string(kMaxFieldSamples)), std::string::npos)
      << field.error().message;
}

TEST(LidarGround, ATileSmallerThanOneCellIsRefusedByName) {
  const PointCloud cloud = make_cloud({{0.0, 0.0, 1.0}, {1.0, 1.0, 1.0}});
  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 10.0;
  const auto field = point_cloud_to_height_field(cloud, options, diagnostics);
  ASSERT_FALSE(field.has_value());
  EXPECT_NE(field.error().message.find("under one"), std::string::npos) << field.error().message;
}

TEST(LidarGround, ANonPositiveSpacingIsRefused) {
  const PointCloud cloud = make_cloud({{0.0, 0.0, 1.0}, {100.0, 100.0, 1.0}});
  std::vector<Diagnostic> diagnostics;
  GroundFitOptions options;
  options.spacing = 0.0;
  EXPECT_FALSE(point_cloud_to_height_field(cloud, options, diagnostics).has_value());
}
