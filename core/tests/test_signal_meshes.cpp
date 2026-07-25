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

// Tests for signal INSTANCE emission in the mesh builder. A <signal> renders as
// an instance of a bundled signal model (props::model), chosen by looking its
// (@country, @type) up in the shipped sign catalogue (roadmaker::signs, spec
// §1.4); an identity no shipped pack claims degrades to "signal_light" or
// "sign_generic" rather than vanishing. The instance carries the signal's world
// pose derived from its s/t/zOffset + hOffset, and the object/signal re-mesh
// channel rebuilds it without touching the road surface. Meshing never mutates
// the network.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/assets/sign_catalog.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// A signal carrying a LEGACY German StVO identity — the one RoadMaker itself
/// authored before the US pack (#414). Nothing in the shipped catalogue matches
/// it, so every test built on this helper is also asserting the degradation
/// path: an unknown identity still meshes, on the generic silhouette.
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

/// A signal authored from the shipped catalogue, exactly the way the editor's
/// make_signal does — so these tests exercise the real pack identities instead
/// of a second spelling of them.
Signal make_pack_signal(std::string odr_id, std::string_view key, double s, double t) {
  const signs::SignDef* def = signs::find_by_key(key);
  EXPECT_NE(def, nullptr) << "no catalogue entry \"" << key << '"';
  Signal sig;
  sig.odr_id = std::move(odr_id);
  sig.s = s;
  sig.t = t;
  if (def != nullptr) {
    sig.dynamic = def->dynamic;
    sig.type = std::string(def->type);
    sig.subtype = std::string(def->subtype);
    sig.country = std::string(def->country);
    sig.text = std::string(def->default_text);
    if (def->default_value.has_value()) {
      sig.value = def->default_value;
      sig.unit = std::string(def->unit);
    }
  }
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

// --- editable text faces (p4-s9, #230; US pack #414) ------------------------

// A D3-1 street-name blade with an editable legend.
Signal make_text_sign(std::string odr_id, double s, double t, std::string text) {
  Signal sig = make_pack_signal(std::move(odr_id), "us.d3_1", s, t);
  sig.text = std::move(text);
  return sig;
}

TEST(SignalMeshes, PackDesignationsResolveTheirModels) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_signal(road, make_pack_signal("n", "us.d3_1", 40.0, 6.0));

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  const signs::SignDef* def = signs::find_by_key("us.d3_1");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(mesh.signal_instances[0].model_id, def->model_id);
}

// The degradation contract. A German StVO plate — the identity RoadMaker wrote
// before #414 — is in no shipped catalogue, so it must still mesh rather than
// vanish. This is what keeps pre-#414 scenes openable.
TEST(SignalMeshes, LegacyIdentityDegradesToTheGenericSilhouette) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal stvo = make_signal("de", /*dynamic=*/false, 40.0, 6.0);
  stvo.type = "206"; // StVO stop sign: used to have its own silhouette
  stvo.subtype = "-1";
  network.add_signal(road, stvo);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_EQ(mesh.signal_instances[0].model_id, "sign_generic");
}

// §1.4: a speed limit's face reads its @value, and it reads it in mph — the
// authored unit — with no conversion and no reference to any display setting.
TEST(SignalMeshes, SpeedLimitFaceComesFromItsValue) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal limit = make_pack_signal("sl", "us.r2_1", 40.0, 6.0);
  ASSERT_TRUE(limit.value.has_value());
  EXPECT_EQ(limit.unit, "mph");
  limit.value = 45.0;
  network.add_signal(road, limit);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  ASSERT_TRUE(mesh.signal_instances[0].face.has_value());
  EXPECT_EQ(mesh.signal_instances[0].face->text, "SPEED\nLIMIT\n45");
}

TEST(SignalMeshes, TextSignCarriesFaceOverlay) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_signal(road, make_text_sign("t", 40.0, 6.0, "City"));

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  const SignalInstance& inst = mesh.signal_instances[0];
  ASSERT_TRUE(inst.face.has_value());
  const SignalFaceOverlay& face = *inst.face;
  EXPECT_EQ(face.text, "City");
  EXPECT_EQ(face.positions.size(), 12U); // 4 verts × xyz
  EXPECT_EQ(face.normals.size(), 12U);
  EXPECT_EQ(face.uvs.size(), 8U);     // 4 verts × uv
  EXPECT_EQ(face.indices.size(), 6U); // 2 triangles
  // Every UV lies in [0,1].
  for (const double uv : face.uvs) {
    EXPECT_GE(uv, 0.0);
    EXPECT_LE(uv, 1.0);
  }
  // Every normal points +x (model-space front face).
  for (std::size_t v = 0; v < 4; ++v) {
    EXPECT_DOUBLE_EQ(face.normals[v * 3 + 0], 1.0);
    EXPECT_DOUBLE_EQ(face.normals[v * 3 + 1], 0.0);
    EXPECT_DOUBLE_EQ(face.normals[v * 3 + 2], 0.0);
  }
  // v = 0 sits at the TOP (higher z) than v = 1 — bitmap row 0 is the top row.
  // Vertex 0 has uv (0,0); vertex 1 has uv (0,1); z0 > z1.
  EXPECT_GT(face.positions[2], face.positions[5]);
}

TEST(SignalMeshes, ABlankPlateNeedsNoFaceTexture) {
  // A street-name blade with no legend has nothing to draw: no artwork, no
  // fixed wording, no @text. It must NOT get a face — a texture of the plate's
  // own flat colour would cost an image per sign and look identical to the
  // plate under it.
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_signal(road, make_text_sign("t", 40.0, 6.0, ""));

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  const signs::SignDef* def = signs::find_by_key("us.d3_1");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(mesh.signal_instances[0].model_id, def->model_id);
  EXPECT_FALSE(mesh.signal_instances[0].face.has_value());
}

// A symbol sign carries no @text at all, and must still show its face — this
// is exactly what the old "non-empty @text" gate got wrong.
TEST(SignalMeshes, ASymbolSignHasAFaceWithNoText) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  network.add_signal(road, make_pack_signal("dne", "us.r5_1", 40.0, 6.0));

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  ASSERT_TRUE(mesh.signal_instances[0].face.has_value());
  EXPECT_TRUE(mesh.signal_instances[0].face->text.empty());
}

TEST(SignalMeshes, DynamicSignalHasNoFace) {
  RoadNetwork network;
  const RoadId road = author_street(network);
  // Even if a dynamic signal carried text, a traffic light shows no text face.
  Signal sig = make_signal("tl", /*dynamic=*/true, 40.0, -6.0);
  sig.text = "ignored";
  network.add_signal(road, sig);

  const NetworkMesh mesh = build_network_mesh(network);
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_EQ(mesh.signal_instances[0].model_id, "signal_light");
  EXPECT_FALSE(mesh.signal_instances[0].face.has_value());
}

TEST(SignalMeshes, ALegacySignsTextStillDrawsOnTheFallbackPlate) {
  // A German StVO 274 plate resolves to the generic silhouette, which now
  // carries a face plate — so its @text is drawn rather than silently dropped
  // as it was before the US pack.
  RoadNetwork network;
  const RoadId road = author_street(network);
  Signal sig = make_signal("g", /*dynamic=*/false, 40.0, 6.0);
  sig.text = "50";
  network.add_signal(road, sig);

  const NetworkMesh mesh = build_network_mesh(network, {});
  ASSERT_EQ(mesh.signal_instances.size(), 1U);
  EXPECT_EQ(mesh.signal_instances[0].model_id, "sign_generic");
  ASSERT_TRUE(mesh.signal_instances[0].face.has_value());
  EXPECT_EQ(mesh.signal_instances[0].face->text, "50");
}

} // namespace
} // namespace roadmaker
