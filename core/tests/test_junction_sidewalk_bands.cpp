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

// Acceptance oracles for issue #402 — junction floor sidewalk band
// segmentation. The 2026-07 field triage measured this defect with a throwaway
// probe harness and set hard numeric criteria from it; per the triage record
// ("the item-1 probe oracles become tests, not GW steps") the measurement lives
// here instead, so the thresholds are enforced on every build.
//
// The four criteria, verbatim from the issue:
//   1. mid-band sidewalk-material misclassification < 1 % per band;
//   2. seam deviation < 0.5 * kSteinerStep from the ideal inner boundary;
//   3. zero-band collapse: 0 cases;
//   4. flush arm faces (no carriageway wedge at a sidewalked mouth).
//
// Everything is measured against `junction_sidewalk_bands()` — the same query
// the mesher constrains its triangulation to, so the oracle is the intent and
// the mesh is the realization.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/fill_params.hpp"
#include "roadmaker/mesh/junction_sidewalk_bands.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

using roadmaker::ContactPoint;
using roadmaker::JunctionFloor;
using roadmaker::JunctionId;
using roadmaker::JunctionSidewalkBand;
using roadmaker::LaneProfile;
using roadmaker::LaneSpec;
using roadmaker::LaneType;
using roadmaker::NetworkMesh;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::SubMesh;
using roadmaker::Waypoint;

namespace {

// --- Fixtures ---------------------------------------------------------------
//
// Deliberately local rather than shared with test_junction_surface.cpp: the
// builders there are pinned to one geometry each (and five_way is hardwired to
// a profile with no sidewalk at all), because they double as the writers for
// the committed .xodr fixtures. The probe matrix needs the arm gap, the arm
// count and the profile to vary, which is a different job.

RoadId author(RoadNetwork& network,
              std::vector<Waypoint> waypoints,
              const char* odr_id,
              const LaneProfile& profile) {
  auto road = roadmaker::author_clothoid_road(network, waypoints, profile, "", odr_id);
  if (!road.has_value()) {
    throw std::runtime_error("author: " + road.error().message);
  }
  return *road;
}

JunctionId make_junction(RoadNetwork& network, const std::vector<RoadEnd>& ends) {
  auto command = roadmaker::edit::create_junction(network, ends);
  if (command == nullptr) {
    throw std::runtime_error("create_junction: null command");
  }
  auto applied = command->apply(network);
  if (!applied.has_value()) {
    throw std::runtime_error("create_junction: " + applied.error().message);
  }
  JunctionId junction;
  network.for_each_junction([&](JunctionId id, const roadmaker::Junction&) { junction = id; });
  return junction;
}

RoadEnd end_of(RoadId road) {
  return RoadEnd{.road = road, .contact = ContactPoint::End};
}

/// `arms` arms radiating from the origin at `angles`, each ending `gap` meters
/// out. `profiles[i]` is arm i's cross section, so a junction can mix
/// sidewalked and plain arms.
JunctionId build_radial(RoadNetwork& network,
                        const std::vector<double>& angles,
                        double gap,
                        const std::vector<LaneProfile>& profiles) {
  std::vector<RoadEnd> ends;
  for (std::size_t i = 0; i < angles.size(); ++i) {
    const double a = angles[i];
    const RoadId arm = author(network,
                              {Waypoint{40.0 * std::cos(a), 40.0 * std::sin(a)},
                               Waypoint{gap * std::cos(a), gap * std::sin(a)}},
                              std::to_string(i + 1).c_str(),
                              profiles[i % profiles.size()]);
    ends.push_back(end_of(arm));
  }
  return make_junction(network, ends);
}

std::vector<double> even_angles(int count) {
  std::vector<double> angles;
  angles.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    angles.push_back((2.0 * std::numbers::pi * static_cast<double>(i)) /
                     static_cast<double>(count));
  }
  return angles;
}

/// A cross section whose sidewalk is NOT the outermost lane: one driving lane,
/// then the sidewalk, then a shoulder outboard of it. Nothing bundled puts a
/// shoulder outside a sidewalk, and that layout used to make the whole junction
/// invisible to the band splitter.
LaneProfile sidewalk_inside_shoulder() {
  LaneProfile profile;
  const LaneSpec driving{.type = LaneType::Driving, .width = roadmaker::defaults::kLocalLaneWidth};
  const LaneSpec walk{.type = LaneType::Sidewalk, .width = roadmaker::defaults::kSidewalkWidth};
  const LaneSpec shoulder{.type = LaneType::Shoulder, .width = roadmaker::defaults::kShoulderWidth};
  profile.left = {driving, walk, shoulder};
  profile.right = {driving, walk, shoulder};
  return profile;
}

// --- Geometry helpers -------------------------------------------------------

using Point = std::array<double, 2>;

double dist(const Point& a, const Point& b) {
  return std::hypot(a[0] - b[0], a[1] - b[1]);
}

Point lerp(const Point& a, const Point& b, double t) {
  return {a[0] + (t * (b[0] - a[0])), a[1] + (t * (b[1] - a[1]))};
}

Point mid_of(const JunctionSidewalkBand& band, std::size_t k) {
  return lerp(band.outer[k], band.inner[k], 0.5);
}

bool in_triangle(const Point& p, const Point& a, const Point& b, const Point& c) {
  const auto side = [](const Point& u, const Point& v, const Point& w) {
    return ((v[0] - u[0]) * (w[1] - u[1])) - ((v[1] - u[1]) * (w[0] - u[0]));
  };
  const double d1 = side(a, b, p);
  const double d2 = side(b, c, p);
  const double d3 = side(c, a, p);
  const bool neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
  const bool pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
  return !(neg && pos);
}

bool covers(const SubMesh& mesh, const Point& p) {
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const auto at = [&mesh](std::uint32_t v) {
      return Point{mesh.positions[v * 3], mesh.positions[(v * 3) + 1]};
    };
    if (in_triangle(p, at(mesh.indices[t]), at(mesh.indices[t + 1]), at(mesh.indices[t + 2]))) {
      return true;
    }
  }
  return false;
}

std::vector<const SubMesh*> sidewalk_bands_of(const JunctionFloor& floor) {
  std::vector<const SubMesh*> bands;
  for (const SubMesh& detail : floor.details) {
    if (detail.material == LaneType::Sidewalk && !detail.indices.empty()) {
      bands.push_back(&detail);
    }
  }
  return bands;
}

enum class Cover { Outside, Carriageway, Sidewalk };

/// What material the finished junction floor actually shows at (x, y).
/// `Outside` means no floor triangle covers the point at all — the ideal band
/// reached past this junction's floor, which the mesher clips away and the
/// oracle therefore skips.
Cover cover_at(const JunctionFloor& floor, const Point& p) {
  for (const SubMesh* band : sidewalk_bands_of(floor)) {
    if (covers(*band, p)) {
      return Cover::Sidewalk;
    }
  }
  return covers(floor.mesh, p) ? Cover::Carriageway : Cover::Outside;
}

/// Distance from `p` to the polyline `line`.
double distance_to_polyline(const Point& p, const std::vector<Point>& line) {
  double best = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    const Point& a = line[i];
    const Point& b = line[i + 1];
    const double ex = b[0] - a[0];
    const double ey = b[1] - a[1];
    const double len2 = (ex * ex) + (ey * ey);
    double t = 0.0;
    if (len2 > 1e-18) {
      t = std::clamp((((p[0] - a[0]) * ex) + ((p[1] - a[1]) * ey)) / len2, 0.0, 1.0);
    }
    best = std::min(best, dist(p, lerp(a, b, t)));
  }
  return best;
}

// --- The probe ---------------------------------------------------------------

struct BandMeasurement {
  std::size_t samples = 0;       ///< mid-band samples that landed on the floor
  std::size_t misclassified = 0; ///< ...and did not show sidewalk material
  double miss_ratio = 0.0;
};

/// Walks one ideal band's MID-LINE and asks the finished mesh what material it
/// shows there. Samples that fall outside the floor are not counted either way.
BandMeasurement measure_band(const JunctionFloor& floor, const JunctionSidewalkBand& band) {
  constexpr double kSampleStep = 0.2;
  BandMeasurement out;
  const std::size_t last = band.outer.size() - 1;
  for (std::size_t k = 0; k + 1 < band.outer.size(); ++k) {
    const Point from = mid_of(band, k);
    const Point to = mid_of(band, k + 1);
    const int steps = std::max(static_cast<int>(std::ceil(dist(from, to) / kSampleStep)), 1);
    for (int s = 0; s <= steps; ++s) {
      // The mid-line's two extreme ends sit exactly ON an arm face, which is
      // the floor's own boundary — no triangle owns a boundary point, so the
      // material there is not a defined quantity. Every other sample counts,
      // including the one 0.2 m in, so a band that fails to reach its mouth is
      // still caught (and criterion 4 probes the mouths directly).
      if ((k == 0 && s == 0) || (k + 1 == last && s == steps)) {
        continue;
      }
      const Point p = lerp(from, to, static_cast<double>(s) / steps);
      const Cover cover = cover_at(floor, p);
      if (cover == Cover::Outside) {
        continue;
      }
      ++out.samples;
      if (cover != Cover::Sidewalk) {
        ++out.misclassified;
      }
    }
  }
  out.miss_ratio = out.samples > 0
                       ? static_cast<double>(out.misclassified) / static_cast<double>(out.samples)
                       : 0.0;
  return out;
}

/// Largest distance from a realized SEAM vertex to the ideal inner boundary.
/// A seam vertex is one the carriageway and a band share bit-for-bit —
/// emit_floor_region copies the floor's exact positions into both regions, so
/// a shared position IS a point on the realized material boundary.
double seam_deviation(const JunctionFloor& floor, const std::vector<JunctionSidewalkBand>& ideal) {
  std::vector<Point> core;
  for (std::size_t v = 0; v + 2 < floor.mesh.positions.size(); v += 3) {
    core.push_back({floor.mesh.positions[v], floor.mesh.positions[v + 1]});
  }
  double worst = 0.0;
  for (const SubMesh* band : sidewalk_bands_of(floor)) {
    for (std::size_t v = 0; v + 2 < band->positions.size(); v += 3) {
      const Point p{band->positions[v], band->positions[v + 1]};
      const bool shared = std::any_of(
          core.begin(), core.end(), [&p](const Point& q) { return q[0] == p[0] && q[1] == p[1]; });
      if (!shared) {
        continue;
      }
      double best = std::numeric_limits<double>::max();
      for (const JunctionSidewalkBand& b : ideal) {
        // The seam against the carriageway is normally the band's INNER
        // boundary, the criterion's subject. A cross section that parks a
        // shoulder outboard of its sidewalk has a second one: the band's OUTER
        // boundary then divides sidewalk from shoulder rather than sidewalk
        // from open air, and it is a realized material boundary exactly like
        // the inner one. Measure each shared vertex against whichever ideal
        // boundary it belongs to.
        best = std::min({best, distance_to_polyline(p, b.inner), distance_to_polyline(p, b.outer)});
      }
      worst = std::max(worst, best);
    }
  }
  return worst;
}

/// One row of the probe matrix.
struct Case {
  std::string name;
  std::vector<double> angles;
  double gap;
  std::vector<LaneProfile> profiles;
};

std::vector<Case> matrix() {
  const LaneProfile walked = LaneProfile::local_road(); // sidewalk both sides
  const LaneProfile plain = LaneProfile::two_lane_rural();
  const LaneProfile shouldered = sidewalk_inside_shoulder();
  const double pi = std::numbers::pi;
  return {
      {"tee_tight", {pi, 0.0, -pi / 2.0}, 6.0, {walked}},
      {"four_way_tight", even_angles(4), 6.0, {walked}},
      {"four_way_roomy", even_angles(4), 20.0, {walked}},
      {"five_way_tight", even_angles(5), 7.0, {walked}},
      {"y_three_way", {pi / 2.0, (7.0 * pi) / 6.0, (11.0 * pi) / 6.0}, 10.0, {walked}},
      {"mixed_sidewalk_and_plain", even_angles(4), 10.0, {walked, walked, walked, plain}},
      {"single_sided", even_angles(4), 10.0, {walked, plain, plain, plain}},
      {"sidewalk_inside_shoulder", even_angles(4), 10.0, {shouldered}},
  };
}

/// The junction floor of a built case, plus the bands it should have.
struct Built {
  RoadNetwork network;
  JunctionId junction;
  NetworkMesh mesh;
};

Built build(const Case& c) {
  Built built;
  built.junction = build_radial(built.network, c.angles, c.gap, c.profiles);
  built.mesh = roadmaker::build_network_mesh(built.network);
  return built;
}

class SidewalkBandMatrix : public testing::TestWithParam<Case> {};

INSTANTIATE_TEST_SUITE_P(Probe,
                         SidewalkBandMatrix,
                         testing::ValuesIn(matrix()),
                         [](const testing::TestParamInfo<Case>& info) { return info.param.name; });

// Criterion 3: every corner with at least one sidewalked adjacent arm emits a
// band. The tight five-way and the sidewalk-inside-shoulder profile used to
// emit NONE junction-wide — a fully sidewalked junction that rendered no
// sidewalk at all.
TEST_P(SidewalkBandMatrix, EverySidewalkedCornerEmitsABand) {
  const Built built = build(GetParam());
  ASSERT_FALSE(built.mesh.junction_floors.empty());
  const std::vector<JunctionSidewalkBand> ideal =
      roadmaker::junction_sidewalk_bands(built.network, built.junction);
  ASSERT_FALSE(ideal.empty()) << "no corner reported a sidewalk band";

  const std::vector<const SubMesh*> realized = sidewalk_bands_of(built.mesh.junction_floors[0]);
  EXPECT_EQ(realized.size(), ideal.size()) << "expected one band per sidewalked corner, got "
                                           << realized.size() << " of " << ideal.size();
  for (const SubMesh* band : realized) {
    EXPECT_EQ(band->material, LaneType::Sidewalk);
    EXPECT_EQ(band->indices.size() % 3, 0U);
    EXPECT_EQ(band->positions.size(), band->normals.size());
  }
}

// Criterion 1: the middle of a band shows sidewalk material. Centroid
// classification against an unconstrained triangulation measured 4 % on the
// roomiest case in the matrix and 27-33 % on the tight ones.
TEST_P(SidewalkBandMatrix, MidBandMisclassificationStaysBelowOnePercent) {
  const Built built = build(GetParam());
  ASSERT_FALSE(built.mesh.junction_floors.empty());
  const JunctionFloor& floor = built.mesh.junction_floors[0];
  const std::vector<JunctionSidewalkBand> ideal =
      roadmaker::junction_sidewalk_bands(built.network, built.junction);
  ASSERT_FALSE(ideal.empty());

  for (std::size_t i = 0; i < ideal.size(); ++i) {
    const BandMeasurement m = measure_band(floor, ideal[i]);
    if (m.samples == 0) {
      continue; // the band lies entirely outside this floor
    }
    EXPECT_LT(m.miss_ratio, 0.01) << "band " << i << ": " << m.misclassified << " of " << m.samples
                                  << " mid-band samples show carriageway material ("
                                  << (100.0 * m.miss_ratio) << " %)";
  }
}

// Criterion 2: the realized seam sits on the ideal one. Constrained edges pin
// it there up to corner-curve tessellation; an unconstrained classification is
// bounded below by the triangle size, which is kSteinerStep.
TEST_P(SidewalkBandMatrix, SeamFollowsTheIdealInnerBoundary) {
  const Built built = build(GetParam());
  ASSERT_FALSE(built.mesh.junction_floors.empty());
  const std::vector<JunctionSidewalkBand> ideal =
      roadmaker::junction_sidewalk_bands(built.network, built.junction);
  ASSERT_FALSE(ideal.empty());

  const double deviation = seam_deviation(built.mesh.junction_floors[0], ideal);
  EXPECT_LT(deviation, 0.5 * roadmaker::fill_params::kSteinerStep)
      << "worst seam vertex sits " << deviation << " m off the ideal inner boundary";
}

// Criterion 4: the band reaches the arm mouth, so the sidewalk pavement of the
// road and the band of the junction meet flush — no carriageway-material wedge
// between them.
TEST_P(SidewalkBandMatrix, BandsAreFlushWithTheArmFaces) {
  const Built built = build(GetParam());
  ASSERT_FALSE(built.mesh.junction_floors.empty());
  const JunctionFloor& floor = built.mesh.junction_floors[0];
  const std::vector<JunctionSidewalkBand> ideal =
      roadmaker::junction_sidewalk_bands(built.network, built.junction);
  ASSERT_FALSE(ideal.empty());

  constexpr double kJustInside = 0.3;
  for (std::size_t i = 0; i < ideal.size(); ++i) {
    const JunctionSidewalkBand& band = ideal[i];
    const std::size_t last = band.outer.size() - 1;
    const std::array<std::pair<Point, Point>, 2> mouths{
        std::pair{mid_of(band, 0), mid_of(band, 1)},
        std::pair{mid_of(band, last), mid_of(band, last - 1)}};
    for (const auto& [face, inward] : mouths) {
      const double run = dist(face, inward);
      if (run < kJustInside) {
        continue; // too short a leg to sample without leaving the band
      }
      const Point probe = lerp(face, inward, kJustInside / run);
      const Cover cover = cover_at(floor, probe);
      if (cover == Cover::Outside) {
        continue;
      }
      EXPECT_EQ(cover, Cover::Sidewalk)
          << "band " << i << " leaves a carriageway wedge at its mouth (" << probe[0] << ", "
          << probe[1] << ")";
    }
  }
}

} // namespace
