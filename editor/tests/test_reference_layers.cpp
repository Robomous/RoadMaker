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

// --- Sidecar round-trip ----------------------------------------------------

TEST(ReferenceLayerSidecar, RoundTripsThroughTheSceneContainer) {
  SceneState state;
  state.reference_layers = std::vector<SceneReferenceLayer>{
      SceneReferenceLayer{.path = "imagery/ortho.tif",
                          .vector = false,
                          .visible = true,
                          .framed_crs = "+proj=tmerc +lat_0=52 +lon_0=5"},
      SceneReferenceLayer{.path = "roads.shp", .vector = true, .visible = false, .framed_crs = ""},
  };

  const Expected<SceneState> parsed = scene_sidecar::parse(scene_sidecar::to_json(state));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 2U);

  EXPECT_EQ((*parsed->reference_layers)[0].path, "imagery/ortho.tif");
  EXPECT_FALSE((*parsed->reference_layers)[0].vector);
  EXPECT_TRUE((*parsed->reference_layers)[0].visible);
  EXPECT_EQ((*parsed->reference_layers)[0].framed_crs, "+proj=tmerc +lat_0=52 +lon_0=5");
  EXPECT_EQ((*parsed->reference_layers)[1].path, "roads.shp");
  EXPECT_TRUE((*parsed->reference_layers)[1].vector);
  EXPECT_FALSE((*parsed->reference_layers)[1].visible);
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
  const QByteArray json =
      R"({"scene_version": 1, "reference_layers": [{"path": "x.tif", "kind": "lidar"}]})";
  const Expected<SceneState> parsed = scene_sidecar::parse(json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->reference_layers.has_value());
  ASSERT_EQ(parsed->reference_layers->size(), 1U);
  EXPECT_FALSE((*parsed->reference_layers)[0].vector);
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
