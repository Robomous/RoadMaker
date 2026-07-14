// Tests for signal INSTANCE emission in the mesh builder. A <signal> renders as
// an instance of a bundled signal model (props::model): dynamic → "signal_light",
// static → "sign_generic". The instance carries the signal's world pose derived
// from its s/t/zOffset + hOffset, and the object/signal re-mesh channel rebuilds
// it without touching the road surface. Meshing never mutates the network.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace roadmaker {
namespace {

RoadId author_street(RoadNetwork& network) {
  const std::vector<Waypoint> waypoints{Waypoint{.x = 0.0, .y = 0.0},
                                        Waypoint{.x = 120.0, .y = 0.0}};
  auto road = author_clothoid_road(network, waypoints, LaneProfile::two_lane_default(), "", "1");
  if (!road.has_value()) {
    throw std::runtime_error("author_street: " + road.error().message);
  }
  return *road;
}

Signal make_signal(std::string odr_id, bool dynamic, double s, double t) {
  Signal sig;
  sig.odr_id = std::move(odr_id);
  sig.dynamic = dynamic;
  sig.type = dynamic ? "1000001" : "274";
  sig.subtype = dynamic ? "-1" : "50";
  sig.country = "DE";
  sig.s = s;
  sig.t = t;
  return sig;
}

TEST(SignalMeshes, BundledSignalModelsExist) {
  const props::PropModel* light = props::model("signal_light");
  const props::PropModel* sign = props::model("sign_generic");
  ASSERT_NE(light, nullptr);
  ASSERT_NE(sign, nullptr);
  EXPECT_GT(light->height, 0.0);
  EXPECT_GT(sign->height, 0.0);
  EXPECT_FALSE(light->parts.empty());
  EXPECT_FALSE(sign->parts.empty());
}

TEST(SignalMeshes, DynamicAndStaticSignalsResolveToTheirModels) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_signal(road, make_signal("tl", /*dynamic=*/true, 30.0, -6.0));
  network.add_signal(road, make_signal("sp", /*dynamic=*/false, 60.0, 6.0));

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 2U);

  const SignalInstance* light = nullptr;
  const SignalInstance* sign = nullptr;
  for (const SignalInstance& inst : mesh.signal_instances) {
    if (inst.model_id == "signal_light") {
      light = &inst;
    } else if (inst.model_id == "sign_generic") {
      sign = &inst;
    }
  }
  ASSERT_NE(light, nullptr) << "the dynamic signal must instance signal_light";
  ASSERT_NE(sign, nullptr) << "the static signal must instance sign_generic";
  EXPECT_EQ(light->road, road);
  EXPECT_EQ(sign->road, road);
}

TEST(SignalMeshes, InstancePoseFollowsStationAndLateralOffset) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  // Straight road along +x: station s maps to x=s, lateral +t maps to +y.
  network.add_signal(road, make_signal("tl", /*dynamic=*/true, 30.0, -6.0));

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  const SignalInstance& inst = mesh.signal_instances[0];
  EXPECT_NEAR(inst.position[0], 30.0, 1e-6);
  EXPECT_NEAR(inst.position[1], -6.0, 1e-6);
  EXPECT_NEAR(inst.position[2], 0.0, 1e-6);
  EXPECT_NEAR(inst.heading, 0.0, 1e-6); // road tangent 0 + hOffset 0
}

TEST(SignalMeshes, HeadingAddsHOffsetAndZOffsetLifts) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sig = make_signal("tl", /*dynamic=*/true, 30.0, -6.0);
  sig.h_offset = 1.5; // arbitrary yaw about +Z
  sig.z_offset = 2.0;
  network.add_signal(road, sig);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_NEAR(mesh.signal_instances[0].heading, 1.5, 1e-6);
  EXPECT_NEAR(mesh.signal_instances[0].position[2], 2.0, 1e-6);
}

TEST(SignalMeshes, RemeshObjectsRebuildsSignalInstances) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  const SignalId id = network.add_signal(road, make_signal("tl", /*dynamic=*/true, 30.0, -6.0));

  NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);

  // Delete the signal and re-mesh only its road via the object channel: the
  // instance must disappear without a full rebuild.
  network.erase_signal(id);
  const std::array<RoadId, 1> dirty{road};
  remesh_objects(network, mesh, dirty, MeshOptions{});
  EXPECT_TRUE(mesh.signal_instances.empty());
}

TEST(SignalMeshes, MissingDynamicFlagDefaultsToSign) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sig = make_signal("s", /*dynamic=*/false, 30.0, -6.0);
  sig.dynamic = std::nullopt; // absent @dynamic
  network.add_signal(road, sig);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_EQ(mesh.signal_instances[0].model_id, "sign_generic");
}

} // namespace
} // namespace roadmaker
