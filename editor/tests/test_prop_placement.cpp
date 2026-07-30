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

// Prop placement helper (p6-s4, issue #238): the pure geometry behind the Prop
// Point and Prop Curve tools. Builds a straight road and asserts road snapping,
// prop-object construction, spacing distribution (including the s=0 sample), the
// off-anchor skip, batch-wide odr-id uniqueness, and the rejection paths.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::RoadNetwork;
using roadmaker::Waypoint;

/// A straight road along the x-axis from (-10, 0) to (10, 0): length 20 m, odr
/// id "1", s = 0 at the west end.
RoadNetwork straight_road() {
  RoadNetwork network;
  auto command = roadmaker::edit::create_road(
      {Waypoint{-10.0, 0.0}, Waypoint{10.0, 0.0}}, roadmaker::LaneProfile::two_lane_rural(), "");
  if (command == nullptr || !command->apply(network).has_value()) {
    throw std::runtime_error("road setup failed");
  }
  return network;
}

LibraryItem pine_item() {
  LibraryItem item;
  item.key = "prop.tree.pine";
  item.label = "Pine tree";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "tree_pine";
  return item;
}

LibraryItem shrub_item() {
  LibraryItem item;
  item.key = "prop.shrub";
  item.label = "Shrub";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "shrub";
  return item;
}

LibraryItem mixed_set_item() {
  // A weighted set: pines three times as likely as shrubs.
  LibraryItem item;
  item.key = "prop_set.mixed";
  item.label = "Mixed";
  item.kind = LibraryItem::Kind::PropSet;
  item.prop_entries.push_back({.model = "tree_pine", .portion = 3.0});
  item.prop_entries.push_back({.model = "shrub", .portion = 1.0});
  return item;
}

// --- the assembly funnels (p6-s9, #323) --------------------------------------
//
// `move_object_command` and `delete_object_command` are what let every editor
// path — the select drag, the Prop Point drag, both gizmo drags, the properties
// spin boxes and Delete — treat a composite prop as one unit without each of
// them learning what an assembly is.

/// A longer road, so an assembly's parts all fit with room to drag.
RoadNetwork assembly_road() {
  RoadNetwork network;
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 200.0, .y = 0.0}};
  auto road = roadmaker::author_clothoid_road(network, waypoints, LaneProfile::urban_sidewalk());
  if (!road.has_value()) {
    throw std::runtime_error("assembly_road: " + road.error().message);
  }
  auto place = edit::place_assembly(network, *road, 60.0, -6.0, 0.0, "signal_mast");
  if (!place->apply(network).has_value()) {
    throw std::runtime_error("assembly_road: place_assembly refused");
  }
  return network;
}

/// The objects on the network, in arena order.
std::vector<ObjectId> all_objects(const RoadNetwork& network) {
  std::vector<ObjectId> out;
  network.for_each_object([&](ObjectId id, const Object&) { out.push_back(id); });
  return out;
}

TEST(PropPlacement, MovingOnePartDragsTheWholeAssemblyWithIt) {
  RoadNetwork network = assembly_road();
  const std::vector<ObjectId> parts = all_objects(network);
  ASSERT_GE(parts.size(), 3U);
  // A part offset from the anchor, so "the grabbed part lands at the cursor" is
  // a real claim rather than one the anchor satisfies for free.
  const ObjectId grabbed = parts[2];
  const double before_t = network.object(grabbed)->t;
  ASSERT_GT(std::abs(before_t - network.object(parts.front())->t), 1.0);

  auto command = move_object_command(network, grabbed, 120.0, before_t);
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(network).has_value());

  // Ghost == commit for the part under the cursor…
  EXPECT_NEAR(network.object(grabbed)->s, 120.0, 1e-9);
  // …and every sibling came along. A funnel that fell through to
  // edit::move_object would have REFUSED instead, so this also proves the route.
  for (const ObjectId id : parts) {
    EXPECT_NEAR(network.object(id)->s, 120.0, 1e-9) << "part left behind";
  }
}

TEST(PropPlacement, MovingAStandalonePropStillGoesThroughMoveObject) {
  RoadNetwork network = assembly_road();
  const RoadId road = network.object(all_objects(network).front())->road;
  Object tree;
  tree.odr_id = "plain";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 20.0;
  tree.t = 8.0;
  const ObjectId plain = network.add_object(road, tree);
  const std::size_t before = network.object_count();

  auto command = move_object_command(network, plain, 30.0, 8.0);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_NEAR(network.object(plain)->s, 30.0, 1e-9);
  // Nothing else moved, and nothing was grouped into it.
  EXPECT_EQ(network.object_count(), before);
  EXPECT_FALSE(network.object(plain)->assembly.has_value());
}

TEST(PropPlacement, DeletingOnePartDeletesTheWholeAssembly) {
  RoadNetwork network = assembly_road();
  const std::vector<ObjectId> parts = all_objects(network);
  ASSERT_GE(parts.size(), 2U);

  auto command = delete_object_command(network, parts[1]);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), 0U) << "a lone pole and arm must not be left standing";

  ASSERT_TRUE(command->revert(network).has_value());
  EXPECT_EQ(network.object_count(), parts.size());
}

TEST(PropPlacement, DeletingAStandalonePropLeavesTheAssemblyAlone) {
  RoadNetwork network = assembly_road();
  const std::size_t assembly_size = network.object_count();
  const RoadId road = network.object(all_objects(network).front())->road;
  Object tree;
  tree.odr_id = "plain";
  tree.name = "tree_pine";
  tree.type = ObjectType::Tree;
  tree.s = 20.0;
  tree.t = 8.0;
  const ObjectId plain = network.add_object(road, tree);

  auto command = delete_object_command(network, plain);
  ASSERT_TRUE(command->apply(network).has_value());
  EXPECT_EQ(network.object_count(), assembly_size);
}

TEST(PropPlacement, PropSetIsPropAsset) {
  EXPECT_TRUE(is_prop_asset(mixed_set_item()));
  // A set with a model that isn't bundled is refused.
  LibraryItem dangling;
  dangling.kind = LibraryItem::Kind::PropSet;
  dangling.prop_entries.push_back({.model = "not_a_model", .portion = 1.0});
  EXPECT_FALSE(is_prop_asset(dangling));
}

TEST(PropPlacement, EmptyPropSetIsNotPlaceable) {
  LibraryItem empty;
  empty.kind = LibraryItem::Kind::PropSet;
  EXPECT_TRUE(empty.prop_entries.empty());
  EXPECT_FALSE(is_prop_asset(empty));
}

TEST(PropPlacement, ResolvePropAssetHonorsPortions) {
  const LibraryItem set = mixed_set_item();

  // A Tree resolves to itself and never advances the RNG.
  std::mt19937 tree_rng(1234);
  const LibraryItem tree = resolve_prop_asset(pine_item(), tree_rng);
  EXPECT_EQ(tree.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(tree.model, "tree_pine");

  // The draw is exactly reproducible for a fixed seed: two independent runs
  // produce a byte-identical model sequence (the seed is not hardcoded to a
  // platform-specific discrete_distribution mapping).
  const auto draw_sequence = [&set](std::uint32_t seed, int n) {
    std::mt19937 rng(seed);
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      const LibraryItem picked = resolve_prop_asset(set, rng);
      EXPECT_EQ(picked.kind, LibraryItem::Kind::Tree);
      out.push_back(picked.model.toStdString());
    }
    return out;
  };
  const std::vector<std::string> first = draw_sequence(42, 400);
  const std::vector<std::string> second = draw_sequence(42, 400);
  EXPECT_EQ(first, second) << "same seed must yield the same draw sequence";

  // Every drawn model is one of the two set entries, and the 3:1 weighting makes
  // pines strictly the majority over a large sample.
  int pines = 0;
  int shrubs = 0;
  for (const std::string& model : first) {
    if (model == "tree_pine") {
      ++pines;
    } else if (model == "shrub") {
      ++shrubs;
    } else {
      ADD_FAILURE() << "unexpected drawn model " << model;
    }
  }
  EXPECT_GT(pines, shrubs);
}

TEST(PropPlacement, SnapAcceptsOnRoadRejectsOpenSpace) {
  const RoadNetwork network = straight_road();
  const auto on = nearest_road_station(network, 0.0, 0.5, kObjectSnapThreshold);
  ASSERT_TRUE(on.has_value());
  EXPECT_EQ(on->road, network.find_road("1"));
  // Far off any road: no snap.
  EXPECT_FALSE(nearest_road_station(network, 0.0, 80.0, kObjectSnapThreshold).has_value());
}

TEST(PropPlacement, IsPropAssetOnlyForTrees) {
  EXPECT_TRUE(is_prop_asset(pine_item()));
  LibraryItem not_a_prop;
  not_a_prop.kind = LibraryItem::Kind::Stencil;
  EXPECT_FALSE(is_prop_asset(not_a_prop));
}

TEST(PropPlacement, MakePropObjectCarriesTypeAndDimensions) {
  const Object tree = make_prop_object(pine_item(), "7", 3.0, -1.0);
  EXPECT_EQ(tree.odr_id, "7");
  EXPECT_EQ(tree.name, "tree_pine");
  EXPECT_EQ(tree.type, ObjectType::Tree);
  EXPECT_DOUBLE_EQ(tree.s, 3.0);
  EXPECT_DOUBLE_EQ(tree.t, -1.0);
  ASSERT_TRUE(tree.radius.has_value());
  ASSERT_TRUE(tree.height.has_value());
  EXPECT_GT(*tree.radius, 0.0);
  EXPECT_GT(*tree.height, 0.0);

  // A shrub is Vegetation rather than Tree.
  const Object shrub = make_prop_object(shrub_item(), "8", 0.0, 0.0);
  EXPECT_EQ(shrub.type, ObjectType::Vegetation);
}

TEST(PropPlacement, DefaultScaleMultipliesModelDimensions) {
  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);

  LibraryItem scaled = pine_item();
  scaled.default_scale = 2.0;
  const Object tree = make_prop_object(scaled, "9", 1.0, 0.0);
  ASSERT_TRUE(tree.radius.has_value());
  ASSERT_TRUE(tree.height.has_value());
  // Both dims scale uniformly (keeps declared data proportional to the render).
  EXPECT_DOUBLE_EQ(*tree.height, model->height * 2.0);
  EXPECT_DOUBLE_EQ(*tree.radius, model->radius * 2.0);

  // A default (1.0) item is unchanged — native model size.
  const Object native = make_prop_object(pine_item(), "10", 1.0, 0.0);
  EXPECT_DOUBLE_EQ(*native.height, model->height);
  EXPECT_DOUBLE_EQ(*native.radius, model->radius);
}

TEST(PropPlacement, ResolvePropAssetCarriesEntryDefaultScale) {
  // A one-entry set with a scaled entry yields a synthetic tree carrying it, so
  // a scatter honors each drawn model's default (filled at arm time).
  LibraryItem set;
  set.kind = LibraryItem::Kind::PropSet;
  set.key = "prop_set.one";
  set.prop_entries.push_back({.model = "tree_pine", .portion = 1.0, .default_scale = 2.0});
  std::mt19937 rng(7);
  const LibraryItem tree = resolve_prop_asset(set, rng);
  EXPECT_EQ(tree.kind, LibraryItem::Kind::Tree);
  EXPECT_EQ(tree.model, "tree_pine");
  EXPECT_DOUBLE_EQ(tree.default_scale, 2.0);
}

TEST(PropPlacement, DistributesEverySpacingIncludingZero) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // 20 m curve, 5 m spacing → props at s = 0, 5, 10, 15, 20 = 5 props.
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_EQ(dist->props.size(), 5U);
  EXPECT_EQ(dist->preview_points.size(), 5U);
  EXPECT_EQ(dist->skipped, 0U);
  // The first prop sits at the west end (s ≈ 0).
  EXPECT_NEAR(dist->props.front().second.s, 0.0, 0.25);
}

TEST(PropPlacement, SpacingChangesCount) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // 10 m spacing → s = 0, 10, 20 = 3 props.
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 10.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_EQ(dist->props.size(), 3U);
}

TEST(PropPlacement, SkipsSamplesThatLeaveTheAnchor) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // A curve that runs perpendicular away from the road: the far samples exceed
  // the lateral snap threshold and are skipped, not relocated.
  const std::vector<Waypoint> points{{-10.0, 0.0}, {-10.0, 30.0}};
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());
  EXPECT_GT(dist->skipped, 0U);
  EXPECT_FALSE(dist->props.empty()); // the near samples still land
}

TEST(PropPlacement, MintsUniqueOdrIdsAcrossBatchAndNetwork) {
  RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // Seed one existing object so the batch must dodge its id.
  auto add =
      roadmaker::edit::add_object(network, anchor, make_prop_object(pine_item(), "1", 5.0, 0.0));
  ASSERT_NE(add, nullptr);
  ASSERT_TRUE(add->apply(network).has_value());

  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  const auto dist = distribute_props_along_curve(network, anchor, points, pine_item(), 5.0);
  ASSERT_TRUE(dist.has_value());

  std::set<std::string> ids;
  for (const auto& [road, object] : dist->props) {
    EXPECT_TRUE(ids.insert(object.odr_id).second) << "duplicate odr id " << object.odr_id;
    EXPECT_NE(object.odr_id, "1") << "reused the existing object's id";
  }
  EXPECT_EQ(ids.size(), dist->props.size());
}

TEST(PropPlacement, RejectsDegenerateInput) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  const std::vector<Waypoint> one{{0.0, 0.0}};
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, one, pine_item(), 5.0).has_value());

  const std::vector<Waypoint> points{{-10.0, 0.0}, {10.0, 0.0}};
  // Non-positive spacing is refused.
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, points, pine_item(), 0.0).has_value());

  // A curve entirely off the anchor road: zero survivors → refused.
  const std::vector<Waypoint> off{{0.0, 80.0}, {20.0, 80.0}};
  EXPECT_FALSE(distribute_props_along_curve(network, anchor, off, pine_item(), 5.0).has_value());
}

// ---- Prop Span (p6-s5) -----------------------------------------------------

TEST(PropPlacement, MakeSpanObjectCarriesOneRepeat) {
  const Expected<Object> span = make_prop_span_object(pine_item(), "3", 15.0, 5.0, -1.5, 4.0);
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->odr_id, "3");
  EXPECT_EQ(span->name, "tree_pine");
  EXPECT_EQ(span->type, ObjectType::Tree);
  // Origin s is the lower of the two stations; the repeat spans the difference.
  EXPECT_DOUBLE_EQ(span->s, 5.0);
  ASSERT_EQ(span->repeats.size(), 1U);
  const ObjectRepeat& repeat = span->repeats.front();
  EXPECT_DOUBLE_EQ(repeat.s, 5.0);
  EXPECT_DOUBLE_EQ(repeat.length, 10.0);
  EXPECT_DOUBLE_EQ(repeat.distance, 4.0);
  EXPECT_DOUBLE_EQ(repeat.t_start, -1.5);
  EXPECT_DOUBLE_EQ(repeat.t_end, -1.5);
  EXPECT_DOUBLE_EQ(repeat.z_offset_start, 0.0);
  EXPECT_DOUBLE_EQ(repeat.z_offset_end, 0.0);
}

TEST(PropPlacement, MakeSpanObjectRejectsBadInput) {
  // Non-positive spacing.
  EXPECT_FALSE(make_prop_span_object(pine_item(), "1", 0.0, 10.0, 0.0, 0.0).has_value());
  // Coincident stations (nothing to span).
  EXPECT_FALSE(make_prop_span_object(pine_item(), "1", 5.0, 5.0, 0.0, 5.0).has_value());
  // A non-prop asset.
  LibraryItem stencil;
  stencil.kind = LibraryItem::Kind::Stencil;
  EXPECT_FALSE(make_prop_span_object(stencil, "1", 0.0, 10.0, 0.0, 5.0).has_value());
}

TEST(PropPlacement, SpanPreviewMatchesExpansion) {
  const RoadNetwork network = straight_road();
  const RoadId anchor = network.find_road("1");
  // A span the full 20 m road at 5 m spacing → instances at s = 0,5,10,15,20.
  const ObjectRepeat repeat = make_span_repeat(0.0, 20.0, 0.0, 5.0);
  const std::vector<std::array<double, 2>> points = span_preview_points(network, anchor, repeat);
  EXPECT_EQ(points.size(), 5U);
  // Every ghost sits on the road (y ≈ 0 for a t = 0 span along the x-axis).
  for (const std::array<double, 2>& point : points) {
    EXPECT_NEAR(point[1], 0.0, 1e-6);
  }
}

// ---- Prop Polygon (p6-s5) --------------------------------------------------

/// A square region straddling the straight road: 16 m wide, 8 m tall, area
/// 128 m², all within kPolygonAnchorMaxT of the road.
std::vector<Waypoint> road_square() {
  return {{-8.0, -4.0}, {8.0, -4.0}, {8.0, 4.0}, {-8.0, 4.0}};
}

TEST(PropPlacement, PolygonScatterDeterministicForSeed) {
  const RoadNetwork network = straight_road();
  const std::vector<Waypoint> region = road_square();
  const auto scatter = [&](std::uint32_t seed) {
    return distribute_props_in_polygon(
        network, region, pine_item(), PropScatterParams{.density_per_100m2 = 10.0, .seed = seed});
  };
  const auto a = scatter(7);
  const auto b = scatter(7);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->props.size(), b->props.size());
  EXPECT_FALSE(a->props.empty());
  for (std::size_t i = 0; i < a->props.size(); ++i) {
    EXPECT_EQ(a->props[i].second.odr_id, b->props[i].second.odr_id);
    EXPECT_EQ(a->props[i].second.name, b->props[i].second.name);
    EXPECT_DOUBLE_EQ(a->props[i].second.s, b->props[i].second.s);
    EXPECT_DOUBLE_EQ(a->props[i].second.t, b->props[i].second.t);
  }
  // A different seed scatters differently (positions or count differ).
  const auto c = scatter(8);
  ASSERT_TRUE(c.has_value());
  bool differs = c->props.size() != a->props.size();
  for (std::size_t i = 0; !differs && i < a->props.size(); ++i) {
    differs = a->props[i].second.s != c->props[i].second.s ||
              a->props[i].second.t != c->props[i].second.t;
  }
  EXPECT_TRUE(differs);
}

TEST(PropPlacement, PolygonDensityScalesCount) {
  const RoadNetwork network = straight_road();
  const std::vector<Waypoint> region = road_square();
  const auto sparse = distribute_props_in_polygon(
      network, region, pine_item(), PropScatterParams{.density_per_100m2 = 4.0, .seed = 1});
  const auto dense = distribute_props_in_polygon(
      network, region, pine_item(), PropScatterParams{.density_per_100m2 = 40.0, .seed = 1});
  ASSERT_TRUE(sparse.has_value());
  ASSERT_TRUE(dense.has_value());
  EXPECT_GT(dense->props.size(), sparse->props.size());
}

TEST(PropPlacement, PolygonSkipsOffRoadSamples) {
  const RoadNetwork network = straight_road();
  // A region 40–70 m off the road: samples beyond kPolygonAnchorMaxT (50 m) have
  // no road in reach and are skipped, while the near band still anchors.
  const std::vector<Waypoint> region{{-8.0, 40.0}, {8.0, 40.0}, {8.0, 70.0}, {-8.0, 70.0}};
  const auto dist = distribute_props_in_polygon(
      network, region, pine_item(), PropScatterParams{.density_per_100m2 = 20.0, .seed = 3});
  ASSERT_TRUE(dist.has_value());
  EXPECT_GT(dist->skipped, 0U);
  EXPECT_FALSE(dist->props.empty());
}

TEST(PropPlacement, PolygonMixesPropSetByPortions) {
  const RoadNetwork network = straight_road();
  const std::vector<Waypoint> region = road_square();
  // A dense scatter of the 3:1 pine/shrub set: pines are the clear majority.
  const auto dist = distribute_props_in_polygon(
      network, region, mixed_set_item(), PropScatterParams{.density_per_100m2 = 50.0, .seed = 5});
  ASSERT_TRUE(dist.has_value());
  int pines = 0;
  int shrubs = 0;
  for (const auto& [road, object] : dist->props) {
    if (object.name == "tree_pine") {
      ++pines;
    } else if (object.name == "shrub") {
      ++shrubs;
    } else {
      ADD_FAILURE() << "unexpected scattered model " << object.name;
    }
  }
  EXPECT_GT(pines + shrubs, 0);
  EXPECT_GT(pines, shrubs);
}

TEST(PropPlacement, PolygonRejectsDegenerateInput) {
  const RoadNetwork network = straight_road();
  // Fewer than three vertices.
  const std::vector<Waypoint> two{{-8.0, -4.0}, {8.0, -4.0}};
  EXPECT_FALSE(
      distribute_props_in_polygon(network, two, pine_item(), PropScatterParams{}).has_value());
  // Three collinear vertices: zero area.
  const std::vector<Waypoint> collinear{{-8.0, 0.0}, {0.0, 0.0}, {8.0, 0.0}};
  EXPECT_FALSE(distribute_props_in_polygon(network, collinear, pine_item(), PropScatterParams{})
                   .has_value());
}

} // namespace
} // namespace roadmaker::editor
