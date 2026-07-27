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

// The transform gizmo's GESTURE (p6-s15, #417), driven headless: which entity a
// selection puts under the gizmo, what each handle edits, and the one-command
// contract. Before this sprint the whole drag lived in ViewportWidget's private
// section, so none of it could be tested at all.
//
// No ViewportWidget here — that is the point of the extraction. Runs under
// QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/signal.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QUndoStack>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "document/gizmo_drag.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp" // station_to_world

namespace roadmaker::editor {
namespace {

using roadmaker::LaneProfile;

constexpr double kSnap = defaults::kPropRotationSnap;

double deg(double radians) {
  return radians * 180.0 / std::numbers::pi;
}

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// One straight road along +x from the origin, so a road-relative heading and a
/// world heading coincide and the sums stay readable.
struct Scene {
  Document document;
  SelectionModel selection{document};
  RoadId road;

  Scene() {
    if (!document.push_command(roadmaker::edit::create_road(
            {{0.0, 0.0}, {120.0, 0.0}}, LaneProfile::two_lane_default(), ""))) {
      throw std::runtime_error("create_road failed");
    }
    document.network().for_each_road([&](RoadId id, const Road&) { road = id; });
  }

  ObjectId add_prop(double s, double t, double hdg) {
    Object object;
    object.odr_id = "1";
    object.type = ObjectType::Tree;
    object.name = "tree_pine"; // a BUNDLED model, so the placement also meshes
    object.s = s;
    object.t = t;
    object.hdg = hdg;
    if (!document.push_command(edit::add_object(document.network(), road, object))) {
      throw std::runtime_error("add_object failed");
    }
    ObjectId placed;
    document.network().for_each_object([&](ObjectId id, const Object&) { placed = id; });
    return placed;
  }

  SignalId add_sign(double s, double t, ObjectOrientation orientation, double h_offset) {
    Signal signal;
    signal.odr_id = "1";
    signal.type = "R1-1";
    signal.country = "US";
    signal.dynamic = false;
    signal.s = s;
    signal.t = t;
    signal.orientation = orientation;
    signal.h_offset = h_offset;
    if (!document.push_command(edit::add_signal(document.network(), road, signal))) {
      throw std::runtime_error("add_signal failed");
    }
    SignalId placed;
    document.network().for_each_signal([&](SignalId id, const Signal&) { placed = id; });
    return placed;
  }

  /// The world point a ring drag must reach for the pivot-relative bearing to
  /// be `bearing`, at a comfortable radius.
  [[nodiscard]] static std::array<double, 2> on_ring(const std::array<double, 3>& pivot,
                                                     double bearing) {
    constexpr double kRadius = 10.0;
    return {pivot[0] + (kRadius * std::cos(bearing)), pivot[1] + (kRadius * std::sin(bearing))};
  }
};

/// Four arms meeting at the origin, welded into one junction. Every road here
/// has a GENERATED pose, which is what the kernel refuses to transform.
struct JunctionScene {
  Document document;
  RoadId west;
  RoadId east;
  RoadId south;
  RoadId north;

  JunctionScene() {
    west = arm(-80.0, 0.0, -20.0, 0.0, "1");
    east = arm(80.0, 0.0, 20.0, 0.0, "2");
    south = arm(0.0, -80.0, 0.0, -20.0, "3");
    north = arm(0.0, 80.0, 0.0, 20.0, "4");
    const std::array<RoadEnd, 4> ends{RoadEnd{west, ContactPoint::End},
                                      RoadEnd{east, ContactPoint::End},
                                      RoadEnd{south, ContactPoint::End},
                                      RoadEnd{north, ContactPoint::End}};
    if (!document.push_command(edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("create_junction failed");
    }
  }

  /// Any one of the junction's generated connecting roads — the only kind of
  /// road a transform still refuses (cascade-s2, #462).
  [[nodiscard]] RoadId a_connecting_road() const {
    RoadId found;
    document.network().for_each_road([&](RoadId id, const Road& r) {
      if (!found.is_valid() && r.junction.is_valid()) {
        found = id;
      }
    });
    return found;
  }

private:
  RoadId arm(double x0, double y0, double x1, double y1, const char* id) {
    if (!document.push_command(
            edit::create_road({{x0, y0}, {x1, y1}}, LaneProfile::two_lane_default(), id))) {
      throw std::runtime_error("create_road failed");
    }
    RoadId road;
    document.network().for_each_road([&](RoadId found, const Road& r) {
      if (r.odr_id == id) {
        road = found;
      }
    });
    return road;
  }
};

/// Scene's single road split in two, so head and tail are genuinely road-road
/// linked — the shape a transform severs. `head` is the half the gizmo grabs.
struct LinkedScene : Scene {
  RoadId head;

  LinkedScene() : head(road) {
    if (!document.push_command(edit::split_road(document.network(), head, 60.0))) {
      throw std::runtime_error("split_road failed");
    }
    if (!document.network().road(head)->successor.has_value()) {
      throw std::runtime_error("split_road left no link");
    }
  }
};

/// Runs a whole ring drag: press at bearing 0, release at `bearing`.
void ring_drag(GizmoDragSession& session,
               const GizmoTarget& target,
               double bearing,
               bool free_rotation = false) {
  ASSERT_EQ(session.begin(target, GizmoHandle::YawRing, Scene::on_ring(target.pivot, 0.0)),
            GizmoDragStart::Armed);
  ASSERT_TRUE(session
                  .update(GizmoDragInput{.cursor_world = Scene::on_ring(target.pivot, bearing),
                                         .free_rotation = free_rotation})
                  .has_value());
  session.commit();
}

} // namespace

// --- which entity the gizmo attaches to -------------------------------------

// The bug p6-s15 fixes. A signal pick carries its owning road (picking.hpp), so
// a resolver that tested `.road` first put the ROAD under the gizmo: selecting
// a sign drew the gizmo at the road midpoint and its ring rotated the whole
// road. Leaf entities must win.
TEST(GizmoTargetResolution, ASelectedSignGizmosItselfAndNotTheRoadItStandsOn) {
  Scene scene;
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, 0.0);

  const auto target = gizmo_target(scene.document.network(), {.road = scene.road, .signal = sign});
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->signal, sign);
  EXPECT_FALSE(target->road.is_valid());

  // ...and the pivot is the sign's own station, not the road's midpoint.
  const Road* road = scene.document.network().road(scene.road);
  ASSERT_NE(road, nullptr);
  const auto expected = station_to_world(road->plan_view, 30.0, -5.0);
  EXPECT_NEAR(target->pivot[0], expected[0], 1e-9);
  EXPECT_NEAR(target->pivot[1], expected[1], 1e-9);
  EXPECT_NE(target->pivot[0], road->plan_view.length() / 2.0);
}

TEST(GizmoTargetResolution, ASelectedPropGizmosItselfAndNotItsRoad) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);

  const auto target = gizmo_target(scene.document.network(), {.road = scene.road, .object = prop});
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->object, prop);
  EXPECT_FALSE(target->road.is_valid());
  EXPECT_FALSE(target->signal.is_valid());
}

TEST(GizmoTargetResolution, ARoadOnlySelectionStillGizmosTheRoad) {
  Scene scene;
  const auto target = gizmo_target(scene.document.network(), {.road = scene.road});
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, scene.road);
  EXPECT_FALSE(target->object.is_valid());
  EXPECT_FALSE(target->signal.is_valid());
}

TEST(GizmoTargetResolution, EntitiesWithNoTransformAndStaleIdsResolveToNothing) {
  Scene scene;
  const RoadNetwork& network = scene.document.network();
  // Junctions and surfaces pick with an INVALID road, so they fall through.
  EXPECT_FALSE(gizmo_target(network, {}).has_value());
  // A stale leaf id must not resolve either — not even to the road beside it.
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  ASSERT_TRUE(scene.document.push_command(edit::delete_object(network, prop)).has_value());
  EXPECT_FALSE(gizmo_target(network, {.road = scene.road, .object = prop}).has_value());
}

// --- rotation: absolute detents ----------------------------------------------

// Absolute, not delta. A prop already at an odd angle lands on an exact
// multiple of the registry increment relative to the road, so a bench ends up
// genuinely perpendicular rather than perpendicular-plus-its-old-offset.
TEST(GizmoDrag, PropRotationSnapsTheResultingHeadingNotTheDelta) {
  Scene scene;
  const double odd = 7.0 * std::numbers::pi / 180.0;
  const ObjectId prop = scene.add_prop(40.0, 4.0, odd);
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);

  // 7° + 20° = 27°, which snaps to 30° — NOT 7° + 15° = 22°.
  const Object* placed = scene.document.network().object(prop);
  ASSERT_NE(placed, nullptr);
  EXPECT_NEAR(deg(placed->hdg), 30.0, 1e-9);
  EXPECT_NEAR(std::remainder(placed->hdg, kSnap), 0.0, 1e-12);
}

TEST(GizmoDrag, TheSuppressionModifierKeepsTheExactAngle) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  const double twenty = 20.0 * std::numbers::pi / 180.0;
  GizmoDragSession session(scene.document);
  ring_drag(session, *target, twenty, /*free_rotation=*/true);

  const Object* placed = scene.document.network().object(prop);
  ASSERT_NE(placed, nullptr);
  EXPECT_NEAR(placed->hdg, twenty, 1e-9);
}

TEST(GizmoDrag, ARotationIsOneUndoEntryRestoringTheExactPriorHeading) {
  Scene scene;
  const double odd = 7.0 * std::numbers::pi / 180.0;
  const ObjectId prop = scene.add_prop(40.0, 4.0, odd);
  const std::string before = xodr(scene.document);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  // Several frames, as a real drag delivers them.
  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::YawRing, Scene::on_ring(target->pivot, 0.0)),
            GizmoDragStart::Armed);
  for (const double bearing : {0.2, 0.5, 20.0 * std::numbers::pi / 180.0}) {
    ASSERT_TRUE(
        session.update(GizmoDragInput{.cursor_world = Scene::on_ring(target->pivot, bearing)})
            .has_value());
  }
  EXPECT_TRUE(session.commit());
  EXPECT_FALSE(session.active());

  EXPECT_EQ(scene.document.undo_stack()->count(), base + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), before);
}

TEST(GizmoDrag, APressThatNeverMovedPushesNothing) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::YawRing, Scene::on_ring(target->pivot, 0.0)),
            GizmoDragStart::Armed);
  EXPECT_FALSE(session.commit()); // no update() ran, so nothing was previewed
  EXPECT_EQ(scene.document.undo_stack()->count(), base);
}

TEST(GizmoDrag, AnAccumulatedRotationStaysWithinOneTurn) {
  Scene scene;
  // Just under half a turn already authored; another near-half-turn drag would
  // push an unwrapped heading past pi, out of the range the Attributes pane's
  // heading spin can show or round-trip.
  const ObjectId prop = scene.add_prop(40.0, 4.0, 3.0);
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 3.0, /*free_rotation=*/true);

  const Object* placed = scene.document.network().object(prop);
  ASSERT_NE(placed, nullptr);
  EXPECT_LE(placed->hdg, std::numbers::pi);
  EXPECT_GT(placed->hdg, -std::numbers::pi);
}

// A span prop's hdg is an offset added to every instance's road-frame heading,
// so one ring drag turns the whole series by the same relative angle. That is
// the intended reading of the field, not an accident.
TEST(GizmoDrag, RotatingASpanPropTurnsTheWholeSeries) {
  Scene scene;
  Object object;
  object.odr_id = "1";
  object.type = ObjectType::Tree;
  object.s = 10.0;
  object.t = 5.0;
  object.repeats.push_back(ObjectRepeat{.s = 10.0, .length = 60.0, .distance = 10.0});
  ASSERT_TRUE(
      scene.document.push_command(edit::add_object(scene.document.network(), scene.road, object))
          .has_value());
  ObjectId span;
  scene.document.network().for_each_object([&](ObjectId id, const Object&) { span = id; });

  const auto target = gizmo_target(scene.document.network(), {.object = span});
  ASSERT_TRUE(target.has_value());
  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);

  const Object* placed = scene.document.network().object(span);
  ASSERT_NE(placed, nullptr);
  EXPECT_NEAR(deg(placed->hdg), 15.0, 1e-9); // 0° + 20° snaps to one increment
  EXPECT_EQ(placed->repeats.size(), 1U);     // the series itself is untouched
}

// --- signs --------------------------------------------------------------------

// @orientation declares which traffic the sign APPLIES to (OpenDRIVE §14.1), so
// a rotation gesture writes the heading offset and nothing else — turning a
// sign must never silently change what it governs.
TEST(GizmoDrag, RotatingASignWritesItsHeadingOffsetAndLeavesOrientationAlone) {
  Scene scene;
  // An authored 7° cant, as auto-orientation's toe-out would leave it.
  const double odd = 7.0 * std::numbers::pi / 180.0;
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, odd);
  const auto target = gizmo_target(scene.document.network(), {.signal = sign});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);

  const Signal* placed = scene.document.network().signal(sign);
  ASSERT_NE(placed, nullptr);
  // Absolute here too: 7° + 20° = 27° snaps to 30°, off the facing datum.
  EXPECT_NEAR(deg(placed->h_offset), 30.0, 1e-9);
  EXPECT_EQ(placed->orientation, ObjectOrientation::Plus);
  EXPECT_DOUBLE_EQ(placed->s, 30.0);
  EXPECT_DOUBLE_EQ(placed->t, -5.0);
}

// The override half of the auto-orientation rule (#416): the ring is not one of
// the three sites allowed to DERIVE a facing, so a heading dragged here is the
// user's and survives everything that is not an explicit re-derivation.
TEST(GizmoDrag, ADraggedSignHeadingSurvivesALaterMoveAndARoundTrip) {
  Scene scene;
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, 0.0);
  const auto target = gizmo_target(scene.document.network(), {.signal = sign});
  ASSERT_TRUE(target.has_value());
  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);
  const double dragged = scene.document.network().signal(sign)->h_offset;

  // Relocating the sign leaves the authored heading exactly as it was.
  ASSERT_TRUE(
      scene.document
          .push_command(edit::move_signal(scene.document.network(), sign, 60.0, -5.0, std::nullopt))
          .has_value());
  EXPECT_DOUBLE_EQ(scene.document.network().signal(sign)->h_offset, dragged);

  // ...and so does a write/read round trip.
  const std::string text = xodr(scene.document);
  const auto reparsed = roadmaker::parse_xodr(text);
  ASSERT_TRUE(reparsed.has_value());
  double round_tripped = 0.0;
  reparsed->network.for_each_signal([&](SignalId, const Signal& s) { round_tripped = s.h_offset; });
  EXPECT_NEAR(round_tripped, dragged, 1e-12);
}

TEST(GizmoDrag, TranslatingASignReprojectsItWithoutTouchingItsHeading) {
  Scene scene;
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, 0.4);
  const auto target = gizmo_target(scene.document.network(), {.signal = sign});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);
  ASSERT_TRUE(
      session
          .update(GizmoDragInput{.cursor_world = {target->pivot[0] + 20.0, target->pivot[1] + 2.0}})
          .has_value());
  EXPECT_TRUE(session.commit());

  const Signal* placed = scene.document.network().signal(sign);
  ASSERT_NE(placed, nullptr);
  EXPECT_NEAR(placed->s, 50.0, 1e-6);
  EXPECT_NEAR(placed->t, -3.0, 1e-6);
  EXPECT_DOUBLE_EQ(placed->h_offset, 0.4);
}

TEST(GizmoDrag, OnlyARoadOffersTheZArm) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, 0.0);
  GizmoDragSession session(scene.document);

  const auto prop_target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(prop_target.has_value());
  EXPECT_EQ(session.begin(*prop_target, GizmoHandle::AxisZ, {0.0, 0.0}), GizmoDragStart::Ignored);
  EXPECT_TRUE(session.refusal().isEmpty());

  const auto sign_target = gizmo_target(scene.document.network(), {.signal = sign});
  ASSERT_TRUE(sign_target.has_value());
  EXPECT_EQ(session.begin(*sign_target, GizmoHandle::AxisZ, {0.0, 0.0}), GizmoDragStart::Ignored);
  EXPECT_TRUE(session.refusal().isEmpty());

  const auto road_target = gizmo_target(scene.document.network(), {.road = scene.road});
  ASSERT_TRUE(road_target.has_value());
  EXPECT_EQ(session.begin(*road_target, GizmoHandle::AxisZ, {0.0, 0.0}), GizmoDragStart::Armed);
  session.cancel();

  EXPECT_EQ(session.begin(*road_target, GizmoHandle::None, {0.0, 0.0}), GizmoDragStart::Ignored);
  EXPECT_TRUE(session.refusal().isEmpty());
}

// --- roads ---------------------------------------------------------------------

// A road is rotated BY a delta about a pivot, so the DELTA is what snaps —
// a road has no single heading to be absolute about.
TEST(GizmoDrag, RoadRotationSnapsTheDelta) {
  Scene scene;
  const auto target = gizmo_target(scene.document.network(), {.road = scene.road});
  ASSERT_TRUE(target.has_value());
  const Road* before = scene.document.network().road(scene.road);
  ASSERT_NE(before, nullptr);
  const double base_hdg = before->plan_view.evaluate(0.0).hdg;

  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);

  const Road* after = scene.document.network().road(scene.road);
  ASSERT_NE(after, nullptr);
  // 20° of drag snaps to one increment of DELTA, not to an absolute bearing.
  EXPECT_NEAR(deg(wrap_angle(after->plan_view.evaluate(0.0).hdg - base_hdg)), deg(kSnap), 1e-6);
}

// #400: the props a road carries must follow it in the MESH, on every preview
// frame — not snap into place on release. The commit path runs with
// already_meshed=true, so a fix that only re-derived on commit would leave the
// whole drag stale; this asserts mid-drag and after release.
TEST(GizmoDrag, PropsAndSignsFollowARoadTranslationEveryFrame) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const SignalId sign = scene.add_sign(30.0, -5.0, ObjectOrientation::Plus, 0.0);

  const auto instance = [&scene, prop] {
    const auto found =
        std::ranges::find(scene.document.mesh().objects, prop, &ObjectInstance::object);
    return found != scene.document.mesh().objects.end() ? *found : ObjectInstance{};
  };
  const auto sign_instance = [&scene, sign] {
    const auto found =
        std::ranges::find(scene.document.mesh().signal_instances, sign, &SignalInstance::signal);
    return found != scene.document.mesh().signal_instances.end() ? *found : SignalInstance{};
  };
  ASSERT_TRUE(instance().object.is_valid()) << "the prop must mesh for this test to mean anything";
  const std::array<double, 3> prop_before = instance().position;
  const std::array<double, 3> sign_before = sign_instance().position;

  const auto target = gizmo_target(scene.document.network(), {.road = scene.road});
  ASSERT_TRUE(target.has_value());
  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::AxisX, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);

  constexpr double kDx = 25.0;
  ASSERT_TRUE(
      session.update(GizmoDragInput{.cursor_world = {target->pivot[0] + kDx, target->pivot[1]}})
          .has_value());
  // Mid-drag: the preview mesh already carries the moved props.
  EXPECT_NEAR(instance().position[0], prop_before[0] + kDx, 1e-9);
  EXPECT_NEAR(instance().position[1], prop_before[1], 1e-9);
  EXPECT_NEAR(sign_instance().position[0], sign_before[0] + kDx, 1e-9);

  session.commit();
  EXPECT_NEAR(instance().position[0], prop_before[0] + kDx, 1e-9);
  EXPECT_NEAR(sign_instance().position[0], sign_before[0] + kDx, 1e-9);

  // ...and undo brings them home.
  scene.document.undo_stack()->undo();
  EXPECT_NEAR(instance().position[0], prop_before[0], 1e-9);
  EXPECT_NEAR(sign_instance().position[0], sign_before[0], 1e-9);
}

TEST(GizmoDrag, PropsRideARoadRotation) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const auto instance = [&scene, prop] {
    const auto found =
        std::ranges::find(scene.document.mesh().objects, prop, &ObjectInstance::object);
    return found != scene.document.mesh().objects.end() ? *found : ObjectInstance{};
  };
  ASSERT_TRUE(instance().object.is_valid());
  const std::array<double, 3> before = instance().position;
  const double heading_before = instance().heading;

  const auto target = gizmo_target(scene.document.network(), {.road = scene.road});
  ASSERT_TRUE(target.has_value());
  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 20.0 * std::numbers::pi / 180.0);

  // The ring snapped the delta to one increment; the prop rode it about the
  // gizmo pivot and turned with the road.
  const double c = std::cos(kSnap);
  const double s = std::sin(kSnap);
  const double ex = target->pivot[0] + (c * (before[0] - target->pivot[0])) -
                    (s * (before[1] - target->pivot[1]));
  const double ey = target->pivot[1] + (s * (before[0] - target->pivot[0])) +
                    (c * (before[1] - target->pivot[1]));
  EXPECT_NEAR(instance().position[0], ex, 1e-6);
  EXPECT_NEAR(instance().position[1], ey, 1e-6);
  EXPECT_NEAR(deg(wrap_angle(instance().heading - heading_before)), deg(kSnap), 1e-6);
}

TEST(GizmoDrag, ACancelledDragLeavesTheDocumentByteIdentical) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const std::string before = xodr(scene.document);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::YawRing, Scene::on_ring(target->pivot, 0.0)),
            GizmoDragStart::Armed);
  ASSERT_TRUE(session.update(GizmoDragInput{.cursor_world = Scene::on_ring(target->pivot, 1.0)})
                  .has_value());
  session.cancel();

  EXPECT_FALSE(session.active());
  EXPECT_EQ(xodr(scene.document), before);
  EXPECT_EQ(scene.document.undo_stack()->count(), base);
}

// --- #401: the gizmo says why it refuses, and asks before it severs ----------

// A junction's CONNECTING road has a GENERATED pose, and the kernel refuses to
// transform it. Before #401 the gizmo armed anyway, failed on every frame, and
// told the user nothing. Now the grab itself is turned away, with the reason —
// and nothing at all is touched.
TEST(GizmoDrag, AConnectingRoadIsRefusedAtTheGrabWithTheKernelsReason) {
  JunctionScene scene;
  const std::string before = xodr(scene.document);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.road = scene.a_connecting_road()});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  EXPECT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Refused);
  // The sentence has to name BOTH ends of the problem, or it is not actionable.
  EXPECT_TRUE(session.refusal().contains("can't be moved")) << session.refusal().toStdString();
  EXPECT_TRUE(session.refusal().contains("Junction")) << session.refusal().toStdString();

  EXPECT_FALSE(session.active());
  EXPECT_EQ(xodr(scene.document), before);
  EXPECT_EQ(scene.document.undo_stack()->count(), base);
}

// The ring is refused too, in the kernel's OWN words for rotation — proving the
// transform kind reaches the wording rather than every refusal reading "moved".
TEST(GizmoDrag, TheYawRingRefusesAConnectingRoadToo) {
  JunctionScene scene;
  const auto target = gizmo_target(scene.document.network(), {.road = scene.a_connecting_road()});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  EXPECT_EQ(session.begin(*target, GizmoHandle::YawRing, Scene::on_ring(target->pivot, 0.0)),
            GizmoDragStart::Refused);
  EXPECT_TRUE(session.refusal().contains("can't be rotated")) << session.refusal().toStdString();
}

// cascade-s2 (#462): an ARM is no longer refused. It moves, and the junction
// regenerates from its new pose — the capability the blanket refusal was
// standing in front of. This is the gate's other half: narrow it too far and
// junction editing breaks, leave it too wide and the feature is unreachable.
TEST(GizmoDrag, AJunctionArmIsMovedNotRefused) {
  JunctionScene scene;
  const std::string before = xodr(scene.document);
  const auto target = gizmo_target(scene.document.network(), {.road = scene.west});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);
  ASSERT_TRUE(session.refusal().isEmpty()) << session.refusal().toStdString();
  ASSERT_TRUE(
      session.update(GizmoDragInput{.cursor_world = {target->pivot[0], target->pivot[1] + 2.0}})
          .has_value());
  EXPECT_TRUE(session.commit());
  EXPECT_NE(xodr(scene.document), before);
}

// The ring carries the WHOLE road selection, which is what makes a rigid
// whole-junction rotation expressible at all: select every arm, grab the ring,
// and the junction turns as one body with its connecting roads.
TEST(GizmoDrag, TheYawRingRotatesTheWholeRoadSelection) {
  JunctionScene scene;
  const std::array<RoadId, 4> selection{scene.west, scene.east, scene.south, scene.north};
  const auto target = gizmo_target(scene.document.network(), {.road = scene.west}, selection);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->roads.size(), 4U) << "the gizmo must carry the selection, not just its road";

  const double east_x_before = scene.document.network().road(scene.east)->plan_view.evaluate(0.0).x;

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::YawRing, Scene::on_ring(target->pivot, 0.0)),
            GizmoDragStart::Armed);
  ASSERT_TRUE(session.update(GizmoDragInput{.cursor_world = Scene::on_ring(target->pivot, 0.4)})
                  .has_value());
  EXPECT_TRUE(session.commit());

  // A road the gizmo was NOT drawn on turned too.
  EXPECT_NE(scene.document.network().road(scene.east)->plan_view.evaluate(0.0).x, east_x_before);
}

// A mid-drag refusal — an arm dragged so far its junction can no longer be
// regenerated from it — must be reported ONCE, not on every mouse-move. Toasting
// per frame is precisely the bug #401 was filed for, and this gesture is now a
// routine way to reach a kernel refusal mid-drag.
TEST(GizmoDrag, AMidDragRefusalIsReportedOnceNotPerFrame) {
  JunctionScene scene;
  const auto target = gizmo_target(scene.document.network(), {.road = scene.west});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);

  // Six frames dragging the arm far out of the junction's reach.
  int reported = 0;
  for (int frame = 0; frame < 6; ++frame) {
    const auto result = session.update(
        GizmoDragInput{.cursor_world = {target->pivot[0], target->pivot[1] - 400.0}});
    EXPECT_TRUE(result.has_value()) << "a refused frame must be absorbed, not returned";
    if (session.take_refusal()) {
      ++reported;
    }
  }
  EXPECT_EQ(reported, 1) << "the refusal must be shown once for the whole drag";
  session.cancel();
}

// The anti-over-gating guard. edit::set_elevation_profile ACCEPTS a junction arm
// — it dirties the junction so it regenerates, which was itself a bug fix — so
// raising an arm with the Z gizmo must keep working. Widening the junction gate
// to AxisZ would look like tidying and would be a regression.
TEST(GizmoDrag, TheZArmStillRaisesAJunctionArm) {
  JunctionScene scene;
  const std::string before = xodr(scene.document);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.road = scene.west});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::AxisZ, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);
  ASSERT_TRUE(
      session
          .update(GizmoDragInput{.cursor_world = {target->pivot[0], target->pivot[1]}, .dz = 3.0})
          .has_value());
  EXPECT_TRUE(session.commit());

  EXPECT_NE(xodr(scene.document), before);
  EXPECT_EQ(scene.document.undo_stack()->count(), base + 1);
}

// cascade-s1 (#461) removed the pre-flight "this will break links" confirmation
// #401 had just wired here: a gizmo move now takes its linked neighbours with
// it, so there is nothing to warn about, and the rare sever that IS unavoidable
// cannot be known at the grab. Document reports those afterwards instead.
TEST(GizmoDrag, DraggingALinkedRoadTakesItsNeighbourWithIt) {
  LinkedScene scene;
  const std::string before = xodr(scene.document);
  const int base = scene.document.undo_stack()->count();
  const auto target = gizmo_target(scene.document.network(), {.road = scene.head});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  // Armed straight away: no dialog, no Declined.
  ASSERT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);
  ASSERT_TRUE(
      session.update(GizmoDragInput{.cursor_world = {target->pivot[0], target->pivot[1] + 20.0}})
          .has_value());
  EXPECT_TRUE(session.commit());

  ASSERT_TRUE(scene.document.network().road(scene.head)->successor.has_value())
      << "the link should have followed, not broken";
  const auto weld = edit::verify_link_weld(
      scene.document.network(), roadmaker::RoadEnd{scene.head, roadmaker::ContactPoint::End});
  ASSERT_TRUE(weld.has_value()) << weld.error().message;
  EXPECT_FALSE(weld->breaches);
  // Move + follow is ONE undo step, and it reverts exactly.
  ASSERT_EQ(scene.document.undo_stack()->count(), base + 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), before);
}

TEST(GizmoDrag, TheYawRingTakesTheNeighbourWithItToo) {
  LinkedScene scene;
  const std::string before = xodr(scene.document);
  const auto target = gizmo_target(scene.document.network(), {.road = scene.head});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ring_drag(session, *target, 10.0 * std::numbers::pi / 180.0);

  ASSERT_TRUE(scene.document.network().road(scene.head)->successor.has_value());
  const auto weld = edit::verify_link_weld(
      scene.document.network(), roadmaker::RoadEnd{scene.head, roadmaker::ContactPoint::End});
  ASSERT_TRUE(weld.has_value()) << weld.error().message;
  EXPECT_FALSE(weld->breaches);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), before);
}

// A prop dragged clear of its road yields no command that frame — deliberately,
// so the prop holds its last valid spot instead of flinging out to a huge t.
// That is NOT an error: Document reports a missing command as one, and if the
// session passed that through, this routine gesture would produce a wall of
// "preview factory returned no command" toasts. Both frames matter: the first
// goes through begin_preview, later ones through update_preview.
TEST(GizmoDrag, DraggingAPropClearOfItsRoadIsNotAnError) {
  Scene scene;
  const ObjectId prop = scene.add_prop(40.0, 4.0, 0.0);
  const auto target = gizmo_target(scene.document.network(), {.object = prop});
  ASSERT_TRUE(target.has_value());

  GizmoDragSession session(scene.document);
  ASSERT_EQ(session.begin(*target, GizmoHandle::PlaneXY, {target->pivot[0], target->pivot[1]}),
            GizmoDragStart::Armed);

  // Frame 1, far off the road: no preview has begun, so this is the
  // begin_preview path.
  EXPECT_TRUE(session.update(GizmoDragInput{.cursor_world = {40.0, 50.0}}).has_value());
  // A good frame, then off the road again: now the update_preview path.
  ASSERT_TRUE(session.update(GizmoDragInput{.cursor_world = {50.0, 4.0}}).has_value());
  EXPECT_TRUE(session.update(GizmoDragInput{.cursor_world = {50.0, 50.0}}).has_value());

  // The last good frame is what survives.
  session.commit();
  const Object* placed = scene.document.network().object(prop);
  ASSERT_NE(placed, nullptr);
  EXPECT_NEAR(placed->s, 50.0, 1.0);
}

} // namespace roadmaker::editor
