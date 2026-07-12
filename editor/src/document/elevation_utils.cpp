#include "document/elevation_utils.hpp"

#include "roadmaker/geometry/poly3.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace roadmaker::editor::elevation {

namespace {

struct Sample {
  double s;
  double x;
  double y;
};

std::vector<Sample> sample_line(const ReferenceLine& line, double step) {
  std::vector<Sample> samples;
  const double length = line.length();
  const int count = std::max(2, static_cast<int>(length / step) + 1);
  samples.reserve(static_cast<std::size_t>(count) + 1);
  for (int i = 0; i <= count; ++i) {
    const double s = length * static_cast<double>(i) / static_cast<double>(count);
    const PathPoint pose = line.evaluate(s);
    samples.push_back(Sample{.s = s, .x = pose.x, .y = pose.y});
  }
  return samples;
}

/// Segment intersection parameters (t, u) in [0, 1] when segments AB and CD
/// properly intersect.
std::optional<std::pair<double, double>>
intersect(const Sample& a, const Sample& b, const Sample& c, const Sample& d) {
  const double rx = b.x - a.x;
  const double ry = b.y - a.y;
  const double sx = d.x - c.x;
  const double sy = d.y - c.y;
  const double denom = rx * sy - ry * sx;
  if (std::abs(denom) < 1e-12) {
    return std::nullopt; // parallel (or degenerate) — no proper crossing
  }
  const double qpx = c.x - a.x;
  const double qpy = c.y - a.y;
  const double t = (qpx * sy - qpy * sx) / denom;
  const double u = (qpx * ry - qpy * rx) / denom;
  if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) {
    return std::nullopt;
  }
  return std::pair{t, u};
}

} // namespace

std::vector<Crossing>
find_crossings(const RoadNetwork& network, RoadId road_id, double end_margin) {
  std::vector<Crossing> crossings;
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return crossings;
  }
  const std::vector<Sample> self = sample_line(road->plan_view, 1.0);

  network.for_each_road([&](RoadId other_id, const Road& other) {
    if (other_id == road_id || other.junction.is_valid()) {
      return;
    }
    const std::vector<Sample> theirs = sample_line(other.plan_view, 1.0);
    for (std::size_t i = 0; i + 1 < self.size(); ++i) {
      for (std::size_t k = 0; k + 1 < theirs.size(); ++k) {
        const auto hit = intersect(self[i], self[i + 1], theirs[k], theirs[k + 1]);
        if (!hit.has_value()) {
          continue;
        }
        const double s_self = self[i].s + hit->first * (self[i + 1].s - self[i].s);
        const double s_other = theirs[k].s + hit->second * (theirs[k + 1].s - theirs[k].s);
        if (s_self < end_margin || s_self > road->length - end_margin || s_other < end_margin ||
            s_other > other.length - end_margin) {
          continue; // near an end this is junction territory, not an overpass
        }
        // Sampled polylines can report one true crossing across two adjacent
        // segment pairs — merge hits closer than the sampling step.
        if (!crossings.empty() && crossings.back().other == other_id &&
            std::abs(crossings.back().s_self - s_self) < 2.0) {
          continue;
        }
        crossings.push_back(Crossing{.other = other_id, .s_self = s_self, .s_other = s_other});
      }
    }
  });
  std::ranges::sort(crossings, {}, &Crossing::s_self);
  return crossings;
}

std::vector<edit::ElevationPoint> overpass_points(
    const RoadNetwork& network, RoadId road_id, bool over, double clearance, double max_grade) {
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return {};
  }
  const std::vector<Crossing> crossings = find_crossings(network, road_id);
  if (crossings.empty()) {
    return edit::elevation_profile_points(*road);
  }

  struct Hump {
    double s;
    double z;
    double ramp;
  };

  std::vector<Hump> humps;
  humps.reserve(crossings.size());
  for (const Crossing& crossing : crossings) {
    const Road* other = network.road(crossing.other);
    const double z_other = eval_profile(other->elevation, crossing.s_other);
    const double z_target = over ? z_other + clearance : z_other - clearance;
    const double z_here = eval_profile(road->elevation, crossing.s_self);
    // Ramp long enough that the climb stays within max_grade: a Hermite
    // segment with zero end tangents peaks at 1.5x its average slope, so the
    // ramp carries that factor (with a floor so even a zero-climb hump keeps
    // a visible plateau).
    const double ramp =
        std::max(20.0, 1.5 * std::abs(z_target - z_here) / std::max(max_grade, 0.01));
    humps.push_back(Hump{.s = crossing.s_self, .z = z_target, .ramp = ramp});
  }

  // Existing nodes survive OUTSIDE every ramp span; the hump owns its span.
  std::vector<edit::ElevationPoint> points;
  for (const edit::ElevationPoint& point : edit::elevation_profile_points(*road)) {
    const bool inside_hump = std::ranges::any_of(humps, [&](const Hump& hump) {
      return point.s > hump.s - hump.ramp && point.s < hump.s + hump.ramp;
    });
    if (!inside_hump) {
      points.push_back(point);
    }
  }
  for (const Hump& hump : humps) {
    const auto anchor = [&](double s) {
      if (s <= 0.0 || s >= road->length) {
        return; // ramp runs past the road end — the crest node still applies
      }
      // Locked zero grade: an estimated tangent at the ramp foot would
      // overshoot the max-grade budget the ramp length was sized for.
      points.push_back(
          edit::ElevationPoint{.s = s, .z = eval_profile(road->elevation, s), .grade = 0.0});
    };
    anchor(hump.s - hump.ramp);
    // Locked zero grade at the crest: the deck is level where it matters.
    points.push_back(edit::ElevationPoint{.s = hump.s, .z = hump.z, .grade = 0.0});
    anchor(hump.s + hump.ramp);
  }
  std::ranges::sort(points, {}, &edit::ElevationPoint::s);
  // Coincident stations can appear when a ramp anchor lands on an existing
  // node station; keep the later (hump-side) one.
  std::vector<edit::ElevationPoint> deduped;
  for (const edit::ElevationPoint& point : points) {
    if (!deduped.empty() && point.s - deduped.back().s <= 1e-3) {
      deduped.back() = point;
    } else {
      deduped.push_back(point);
    }
  }
  return deduped;
}

} // namespace roadmaker::editor::elevation
