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

// Imported GIS reference layers in the editor (p7-s2, #242): the Layer-2
// model, its sidecar round-trip, and the geometry the viewport uploads.
//
// Headless — nothing here touches GL or a window.

#include "roadmaker/gis/layer.hpp"
#include "roadmaker/road/georeference.hpp"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "document/reference_layers.hpp"
#include "document/scene_sidecar.hpp"
#include "render/scene_builder.hpp"

using namespace roadmaker;
using namespace roadmaker::editor;

namespace {

std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path(RM_GIS_FIXTURES_DIR) / name;
}

std::filesystem::path lidar_fixture(const std::string& name) {
  return std::filesystem::path(RM_LIDAR_FIXTURES_DIR) / name;
}

GeoReference amsterdam() {
  GeoReference geo;
  geo.projection = *tmerc_projection(52.3702, 4.8952);
  return geo;
}

} // namespace

// --- The model -------------------------------------------------------------

TEST(ReferenceLayers, ImportingAShapefilePlacesItInTheSceneFrame) {
  ReferenceLayers layers;
  const Expected<std::vector<Diagnostic>> added =
      layers.add(fixture("utm31_roads.shp"), fixture("").parent_path(), amsterdam());
  ASSERT_TRUE(added.has_value()) << added.error().message;

  ASSERT_EQ(layers.size(), 1U);
  const ReferenceLayer& layer = layers.at(0);
  EXPECT_EQ(layer.kind, ReferenceLayerKind::Vector);
  EXPECT_TRUE(layer.loaded);
  EXPECT_TRUE(layer.visible);
  EXPECT_NE(layer.status.find("UTM zone 31N"), std::string::npos) << layer.status;
  // Placed relative to the scene origin, not still carrying raw UTM eastings.
  EXPECT_LT(std::abs(layer.vector.bounds[0]), 20000.0);
}

TEST(ReferenceLayers, ImportingAnOutOfFamilyCrsFailsAndNamesIt) {
  ReferenceLayers layers;
  const Expected<std::vector<Diagnostic>> added =
      layers.add(fixture("unsupported_crs_roads.shp"), fixture("").parent_path(), amsterdam());
  ASSERT_FALSE(added.has_value());
  EXPECT_NE(added.error().message.find("NAD27_Texas_North"), std::string::npos)
      << added.error().message;
  // A layer that could not be placed is NOT added — the list only ever holds
  // things the user can actually see.
  EXPECT_TRUE(layers.empty());
}

TEST(ReferenceLayers, AnElevationRasterIsRefusedAsABackdropAndSaysWhereToGo) {
  // It would otherwise draw a greyscale height map over the ground and leave
  // the user wondering why it affects nothing.
  ReferenceLayers layers;
  const Expected<std::vector<Diagnostic>> added =
      layers.add(fixture("utm31_dem.tif"), fixture("").parent_path(), amsterdam());
  ASSERT_FALSE(added.has_value());
  EXPECT_NE(added.error().message.find("Terrain"), std::string::npos) << added.error().message;
}

TEST(ReferenceLayers, PathsAreStoredRelativeToTheScene) {
  // The contract every external reference in this repository keeps: a project
  // copied to another machine, or committed, must keep working.
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(fixture("utm31_image.tif"), fixture("").parent_path(), amsterdam()).has_value());
  const std::string& path = layers.at(0).path;
  EXPECT_EQ(path, "utm31_image.tif");
  EXPECT_FALSE(std::filesystem::path(path).is_absolute());
}

TEST(ReferenceLayers, AMissingSourceComesBackUnloadedRatherThanDropped) {
  // Losing a reference the user added because a drive was not mounted is worse
  // than showing it as unavailable.
  ReferenceLayer stale;
  stale.path = "gone.geojson";
  stale.kind = ReferenceLayerKind::Vector;

  ReferenceLayers layers;
  const std::vector<Diagnostic> diagnostics =
      layers.reload({stale}, fixture("").parent_path(), amsterdam());

  ASSERT_EQ(layers.size(), 1U);
  EXPECT_FALSE(layers.at(0).loaded);
  EXPECT_FALSE(layers.at(0).drawable());
  EXPECT_FALSE(diagnostics.empty());
}

TEST(ReferenceLayers, AChangedGeoreferenceReDerivesRatherThanDiscards) {
  // ★ The difference from the workspace box, and the reason for it: a
  // reference layer has a SOURCE FILE to re-derive from, so a changed frame
  // re-places it. The workspace box has no source, which is why that one is
  // discarded instead.
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(fixture("utm31_roads.shp"), fixture("").parent_path(), amsterdam()).has_value());
  const double before = layers.at(0).vector.bounds[0];

  GeoReference moved;
  moved.projection = *tmerc_projection(48.8566, 2.3522); // Paris
  const std::vector<Diagnostic> diagnostics = layers.refit(fixture("").parent_path(), moved);

  ASSERT_EQ(layers.size(), 1U);
  EXPECT_TRUE(layers.at(0).loaded) << "a re-framed layer must survive, not vanish";
  EXPECT_NE(layers.at(0).vector.bounds[0], before) << "and it must actually move";
  EXPECT_EQ(layers.at(0).framed_crs, moved.projection);
  (void)diagnostics;
}

TEST(ReferenceLayers, HiddenLayersAreNotDrawableAndDoNotContributeBounds) {
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(fixture("utm31_image.tif"), fixture("").parent_path(), amsterdam()).has_value());
  ASSERT_TRUE(layers.bounds().has_value());

  layers.set_visible(0, false);
  EXPECT_FALSE(layers.at(0).drawable());
  EXPECT_FALSE(layers.bounds().has_value());
}

// --- Point clouds (p7-s3, #243) --------------------------------------------

TEST(ReferenceLayers, ImportingALidarTilePlacesItAndReportsWhatItRead) {
  ReferenceLayers layers;
  const Expected<std::vector<Diagnostic>> added =
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam());
  ASSERT_TRUE(added.has_value()) << added.error().message;
  ASSERT_EQ(layers.size(), 1U);

  const ReferenceLayer& layer = layers.at(0);
  EXPECT_EQ(layer.kind, ReferenceLayerKind::PointCloud)
      << "the reader's own extension predicate decides the kind, not the menu entry";
  EXPECT_TRUE(layer.loaded);
  EXPECT_FALSE(layer.cloud.empty());
  // Placed in the SCENE's frame: the tile is UTM, the scene is a tmerc on
  // Amsterdam, so a cloud still carrying UTM eastings was never reprojected.
  EXPECT_LT(std::abs(layer.cloud.bounds[0]), 100000.0)
      << "the cloud is still at UTM magnitude, so it was not moved into the scene frame";
  // The status line is what the user reads, and it must name the CRS.
  EXPECT_NE(layer.status.find("UTM"), std::string::npos) << layer.status;
}

TEST(ReferenceLayers, ALidarTileInAnUnsupportedCrsIsRefusedByName) {
  // The refusal is gis::crs_transform's, uncopied, so a point cloud, a raster
  // and a vector all say the same thing about the same CRS.
  ReferenceLayers layers;
  const Expected<std::vector<Diagnostic>> added = layers.add(
      lidar_fixture("unsupported_crs.las"), lidar_fixture("").parent_path(), amsterdam());
  ASSERT_FALSE(added.has_value());
  EXPECT_NE(added.error().message.find("485"), std::string::npos) << added.error().message;
  EXPECT_EQ(layers.size(), 0U);
}

TEST(ReferenceLayers, AChangedGeoreferenceReDerivesACloudToo) {
  // The third kind must inherit the re-derive rule, not just the two that
  // shipped with #242.
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam())
          .has_value());
  const double before = layers.at(0).cloud.bounds[0];

  GeoReference moved;
  moved.projection = *tmerc_projection(48.8566, 2.3522); // Paris
  (void)layers.refit(lidar_fixture("").parent_path(), moved);

  ASSERT_EQ(layers.size(), 1U);
  EXPECT_TRUE(layers.at(0).loaded) << "a re-framed cloud must survive, not vanish";
  EXPECT_FALSE(layers.at(0).cloud.empty());
  EXPECT_NE(layers.at(0).cloud.bounds[0], before) << "and it must actually move";
  EXPECT_EQ(layers.at(0).framed_crs, moved.projection);
}

TEST(ReferenceLayers, ACloudContributesItsPlanViewBoundsForFraming) {
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam())
          .has_value());
  const auto box = layers.bounds();
  ASSERT_TRUE(box.has_value()) << "a cloud must be framable, or Frame Selection ignores it";
  // Plan view: x/y of the 3D bounds, never z.
  EXPECT_DOUBLE_EQ((*box)[0], layers.at(0).cloud.bounds[0]);
  EXPECT_DOUBLE_EQ((*box)[1], layers.at(0).cloud.bounds[1]);
  EXPECT_DOUBLE_EQ((*box)[2], layers.at(0).cloud.bounds[3]);
  EXPECT_DOUBLE_EQ((*box)[3], layers.at(0).cloud.bounds[4]);
}

TEST(SceneBuilderCloud, PointsAreNotDrawnAsLines) {
  // ★ THE GATE FOR gl_renderer's PRIMITIVE SWITCH. That was a two-way ternary
  // (`kind == Triangles ? kTriangles : kLines`), so a third PrimitiveKind drew
  // as LINE SEGMENTS BETWEEN CONSECUTIVE POINTS — silently, with no warning and
  // no error, just a cloud rendered as garbage.
  //
  // This test cannot reach GL headlessly, so it pins the input to that switch:
  // the mesh must actually claim to be Points. A build where cloud_points()
  // returned Lines would draw identically to the bug.
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam())
          .has_value());
  const lidar::PointCloud& cloud = layers.at(0).cloud;

  const RenderMeshData mesh =
      cloud_points(cloud, static_cast<float>(cloud.bounds[2]), static_cast<float>(cloud.bounds[5]));
  EXPECT_EQ(mesh.kind, PrimitiveKind::Points);
  EXPECT_EQ(mesh.positions.size(), cloud.size() * 3);
  // upload() returns a null handle for an index-less mesh, so a cloud without
  // one index per point would not draw at all.
  EXPECT_EQ(mesh.indices.size(), cloud.size());
  EXPECT_EQ(mesh.uvs.size(), cloud.size() * 2);
}

TEST(SceneBuilderCloud, PositionsStayInTheCloudsOwnFrame) {
  // ★ The offset representation has to survive all the way to the GPU. Baking
  // cloud.origin into the vertices here would put a scene-scale value back into
  // a float — the same defect the kernel's own precision test guards, one layer
  // further out. The translation travels as the draw's InstanceData instead.
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam())
          .has_value());
  const lidar::PointCloud& cloud = layers.at(0).cloud;

  const RenderMeshData mesh =
      cloud_points(cloud, static_cast<float>(cloud.bounds[2]), static_cast<float>(cloud.bounds[5]));
  ASSERT_FALSE(mesh.positions.empty());
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    EXPECT_FLOAT_EQ(mesh.positions[i], cloud.xyz[i]) << "at " << i;
  }
}

TEST(SceneBuilderCloud, TheHeightRampSpansTheCloudAndClampsOutsideIt) {
  ReferenceLayers layers;
  ASSERT_TRUE(
      layers.add(lidar_fixture("amsterdam_tile.laz"), lidar_fixture("").parent_path(), amsterdam())
          .has_value());
  const lidar::PointCloud& cloud = layers.at(0).cloud;

  const RenderMeshData mesh =
      cloud_points(cloud, static_cast<float>(cloud.bounds[2]), static_cast<float>(cloud.bounds[5]));
  float low = 1.0F;
  float high = 0.0F;
  for (std::size_t i = 0; i < mesh.uvs.size(); i += 2) {
    EXPECT_GE(mesh.uvs[i], 0.0F);
    EXPECT_LE(mesh.uvs[i], 1.0F);
    low = std::min(low, mesh.uvs[i]);
    high = std::max(high, mesh.uvs[i]);
  }
  EXPECT_NEAR(low, 0.0F, 1e-3F) << "the lowest point must sit at the bottom of the ramp";
  EXPECT_NEAR(high, 1.0F, 1e-3F) << "and the highest at the top";
}

TEST(SceneBuilderCloud, AFlatCloudDoesNotDivideByItsOwnZeroRange) {
  // A car park, or a single scan line. Normalising over a zero span would emit
  // NaN uvs and sample the ramp at an undefined texel.
  lidar::PointCloud flat;
  flat.origin = {0.0, 0.0, 0.0};
  flat.xyz = {0.0F, 0.0F, 5.0F, 1.0F, 0.0F, 5.0F, 0.0F, 1.0F, 5.0F};
  flat.bounds = {0.0, 0.0, 5.0, 1.0, 1.0, 5.0};

  const RenderMeshData mesh = cloud_points(flat, 5.0F, 5.0F);
  ASSERT_EQ(mesh.uvs.size(), 6U);
  for (std::size_t i = 0; i < mesh.uvs.size(); i += 2) {
    EXPECT_TRUE(std::isfinite(mesh.uvs[i]));
    EXPECT_FLOAT_EQ(mesh.uvs[i], 0.5F);
  }
}

TEST(SceneBuilderCloud, TheRampIsAOneRowClampedTexture) {
  const TextureData ramp = cloud_ramp_texture();
  EXPECT_EQ(ramp.width, 256);
  EXPECT_EQ(ramp.height, 1);
  // Repeat would wrap the ramp's own ends back over each other at the extremes.
  EXPECT_EQ(ramp.wrap, TextureWrap::ClampToEdge);
  ASSERT_EQ(ramp.rgba.size(), 256U * 4U);
  // Actually a ramp, not a flat fill: the ends must differ.
  EXPECT_NE(ramp.rgba[0], ramp.rgba[255U * 4U]);
}

// --- Sidecar round-trip ----------------------------------------------------

TEST(ReferenceLayerSidecar, RoundTripsThroughTheSceneContainer) {
  SceneState state;
  // All THREE kinds, because the persisted `kind` stopped being a bool in
  // p7-s3 (#243) and a two-of-three round trip would not notice a third
  // spelling collapsing onto one of the others.
  state.reference_layers = std::vector<SceneReferenceLayer>{
      SceneReferenceLayer{.path = "imagery/ortho.tif",
                          .kind = ReferenceLayerKind::Raster,
                          .visible = true,
                          .framed_crs = "+proj=tmerc +lat_0=52 +lon_0=5"},
      SceneReferenceLayer{.path = "roads.shp",
                          .kind = ReferenceLayerKind::Vector,
                          .visible = false,
                          .framed_crs = ""},
      SceneReferenceLayer{.path = "survey/tile.laz",
                          .kind = ReferenceLayerKind::PointCloud,
                          .visible = true,
                          .framed_crs = "+proj=utm +zone=31"},
  };

  const Expected<SceneState> parsed = scene_sidecar::parse(scene_sidecar::to_json(state));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 3U);

  EXPECT_EQ((*parsed->reference_layers)[0].path, "imagery/ortho.tif");
  EXPECT_EQ((*parsed->reference_layers)[0].kind, ReferenceLayerKind::Raster);
  EXPECT_TRUE((*parsed->reference_layers)[0].visible);
  EXPECT_EQ((*parsed->reference_layers)[0].framed_crs, "+proj=tmerc +lat_0=52 +lon_0=5");
  EXPECT_EQ((*parsed->reference_layers)[1].path, "roads.shp");
  EXPECT_EQ((*parsed->reference_layers)[1].kind, ReferenceLayerKind::Vector);
  EXPECT_FALSE((*parsed->reference_layers)[1].visible);
  EXPECT_EQ((*parsed->reference_layers)[2].path, "survey/tile.laz");
  EXPECT_EQ((*parsed->reference_layers)[2].kind, ReferenceLayerKind::PointCloud);
  EXPECT_EQ((*parsed->reference_layers)[2].framed_crs, "+proj=utm +zone=31");
}

TEST(ReferenceLayerSidecar, ASidecarWrittenBeforeThereWasAThirdKindStillLoads) {
  // The on-disk spellings "vector" and "raster" predate #243, and a scene saved
  // by an older build must keep opening. The new spelling has to be a NEW word
  // rather than a redefinition of either.
  const QByteArray json = R"({
    "scene_version": 1,
    "reference_layers": [
      {"path": "old.tif", "kind": "raster", "visible": true},
      {"path": "old.shp", "kind": "vector", "visible": true}
    ]
  })";
  const Expected<SceneState> parsed = scene_sidecar::parse(json);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 2U);
  EXPECT_EQ((*parsed->reference_layers)[0].kind, ReferenceLayerKind::Raster);
  EXPECT_EQ((*parsed->reference_layers)[1].kind, ReferenceLayerKind::Vector);
}

TEST(ReferenceLayerSidecar, AMalformedEntryIsSkippedAndTheRestSurvive) {
  // Per-entry rather than all-or-nothing, unlike `view` and `workspace`:
  // dropping five good layers because a sixth is malformed helps nobody.
  const QByteArray json = R"({
    "scene_version": 1,
    "reference_layers": [
      {"path": "good.tif", "kind": "raster"},
      {"kind": "raster"},
      "not an object",
      {"path": "also_good.shp", "kind": "vector"}
    ]
  })";
  const Expected<SceneState> parsed = scene_sidecar::parse(json);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 2U);
  EXPECT_EQ((*parsed->reference_layers)[0].path, "good.tif");
  EXPECT_EQ((*parsed->reference_layers)[1].path, "also_good.shp");
}

TEST(ReferenceLayerSidecar, AnUnknownKindReadsAsRasterRatherThanBeingDropped) {
  // "lidar" was the placeholder here until #243 made point clouds real. It is
  // deliberately NOT the spelling that shipped ("point_cloud"), so this test
  // still means what its name says.
  const QByteArray json =
      R"({"scene_version": 1, "reference_layers": [{"path": "x.tif", "kind": "hologram"}]})";
  const Expected<SceneState> parsed = scene_sidecar::parse(json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 1U);
  EXPECT_EQ((*parsed->reference_layers)[0].kind, ReferenceLayerKind::Raster);
}

TEST(ReferenceLayerSidecar, NoLayersWritesNoBlockAtAll) {
  SceneState state;
  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(state)).object();
  EXPECT_FALSE(root.contains("reference_layers"));
}

TEST(ReferenceLayerSidecar, UnknownKeysStillSurviveAlongsideTheNewBlock) {
  // The forward-compat merge must keep working now that a second array-valued
  // block shares the root.
  const QByteArray json =
      R"({"scene_version": 1, "session": {"note": "keep me"},
          "reference_layers": [{"path": "x.tif", "kind": "raster"}]})";
  const Expected<SceneState> parsed = scene_sidecar::parse(json);
  ASSERT_TRUE(parsed.has_value());
  const QJsonObject root = QJsonDocument::fromJson(scene_sidecar::to_json(*parsed)).object();
  ASSERT_TRUE(root.contains("session"));
  EXPECT_EQ(root.value("session").toObject().value("note").toString(), QStringLiteral("keep me"));
  EXPECT_EQ(root.value("reference_layers").toArray().size(), 1);
}

// --- The geometry the viewport uploads -------------------------------------

TEST(ReferenceLayerGeometry, TheUnderlayQuadFacesUpAndCarriesFullUvs) {
  const RenderMeshData quad = underlay_quad({10.0, 20.0, 30.0, 50.0}, 1.5F);
  ASSERT_EQ(quad.positions.size(), 12U);
  EXPECT_EQ(quad.indices.size(), 6U);
  EXPECT_EQ(quad.kind, PrimitiveKind::Triangles);

  for (std::size_t i = 2; i < quad.positions.size(); i += 3) {
    EXPECT_FLOAT_EQ(quad.positions[i], 1.5F);
  }
  for (std::size_t i = 2; i < quad.normals.size(); i += 3) {
    EXPECT_FLOAT_EQ(quad.normals[i], 1.0F) << "a back-facing underlay is invisible";
  }
  // Row 0 of an image is its NORTH edge, so v must be 0 at MAX y. The other way
  // up flips every imported orthophoto — plausible until compared to a road.
  ASSERT_EQ(quad.uvs.size(), 8U);
  EXPECT_FLOAT_EQ(quad.uvs[1], 1.0F); // (min_x, min_y) -> bottom of the image
  EXPECT_FLOAT_EQ(quad.uvs[5], 0.0F); // (max_x, max_y) -> top of the image
}

TEST(ReferenceLayerGeometry, VectorLinesConnectRunsAndCloseRingsExactlyOnce) {
  gis::GisVectorLayer layer;

  gis::GisFeature line;
  line.geometry = gis::GisFeature::Geometry::Line;
  line.ring_starts = {0};
  line.vertices = {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}};
  layer.features.push_back(line);

  // A ring that does NOT repeat its first vertex must be closed by the builder.
  gis::GisFeature open_ring;
  open_ring.geometry = gis::GisFeature::Geometry::Polygon;
  open_ring.ring_starts = {0};
  open_ring.vertices = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}};
  layer.features.push_back(open_ring);

  // A ring that DOES repeat it must not gain a zero-length segment.
  gis::GisFeature closed_ring;
  closed_ring.geometry = gis::GisFeature::Geometry::Polygon;
  closed_ring.ring_starts = {0};
  closed_ring.vertices = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 0.0}};
  layer.features.push_back(closed_ring);

  const RenderMeshData mesh = underlay_lines(layer, 0.25F, {1.0F, 1.0F, 1.0F, 1.0F});
  EXPECT_EQ(mesh.kind, PrimitiveKind::Lines);
  // 2 segments (line) + 3 (open ring, closed) + 3 (closed ring, not re-closed)
  EXPECT_EQ(mesh.indices.size(), (2U + 3U + 3U) * 2U);
  for (std::size_t i = 2; i < mesh.positions.size(); i += 3) {
    EXPECT_FLOAT_EQ(mesh.positions[i], 0.25F);
  }
}

TEST(ReferenceLayerGeometry, APointFeatureDrawsAVisibleCross) {
  // A single GL point is invisible at most zooms, and a point layer that draws
  // nothing reads as an import that silently failed.
  gis::GisVectorLayer layer;
  gis::GisFeature point;
  point.geometry = gis::GisFeature::Geometry::Point;
  point.ring_starts = {0};
  point.vertices = {{5.0, 5.0}};
  layer.features.push_back(point);

  const RenderMeshData mesh = underlay_lines(layer, 0.0F, {1.0F, 1.0F, 1.0F, 1.0F});
  EXPECT_EQ(mesh.indices.size(), 4U); // two crossing segments
  EXPECT_EQ(mesh.positions.size(), 12U);
}

TEST(ReferenceLayerGeometry, LayersStepApartSoAStackIsOrderedNotZFighting) {
  SceneBounds bounds;
  bounds.lo = {0.0F, 0.0F, 0.0F};
  bounds.hi = {100.0F, 100.0F, 5.0F};
  EXPECT_LT(underlay_z(bounds, 0), underlay_z(bounds, 1));
  EXPECT_LT(underlay_z(bounds, 1), underlay_z(bounds, 2));
  // And above the procedural ground, so an underlay hides the grass it covers.
  EXPECT_GT(underlay_z(bounds, 0), ground_base_z(bounds));
}
