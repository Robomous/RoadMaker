#include "roadmaker/edit/snap.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace roadmaker::edit {

namespace {

double distance(Waypoint a, Waypoint b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

/// Wraps to [-pi, pi].
double wrap_angle(double angle) {
  return std::remainder(angle, 2.0 * std::numbers::pi);
}

struct Candidate {
  SnapResult result;
  double dist = 0.0;
};

/// Keeps `candidate` when it is in range and strictly closer than `best` —
/// strict so equidistant later candidates never displace earlier ones.
void consider(std::optional<Candidate>& best, Candidate candidate, double radius) {
  if (candidate.dist > radius) {
    return;
  }
  if (!best || candidate.dist < best->dist) {
    best = std::move(candidate);
  }
}

} // namespace

std::optional<SnapResult>
snap_point(const RoadNetwork& network, Waypoint cursor, const SnapOptions& options) {
  std::optional<Candidate> best_endpoint;
  std::optional<Candidate> best_tangent;

  if (options.endpoints || options.tangent) {
    network.for_each_road([&](RoadId id, const Road& road) {
      if (options.exclude_road && *options.exclude_road == id) {
        return;
      }
      const ReferenceLine& line = road.plan_view;
      if (line.length() <= tol::kLength) {
        return;
      }
      for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
        const bool at_end = contact == ContactPoint::End;
        const PathPoint pose = line.evaluate(at_end ? line.length() : 0.0);
        const Waypoint end_pos{.x = pose.x, .y = pose.y};
        const double away = at_end ? pose.hdg : wrap_angle(pose.hdg + std::numbers::pi);

        if (options.endpoints) {
          consider(best_endpoint,
                   Candidate{
                       .result = {.position = end_pos,
                                  .heading = away,
                                  .kind = SnapKind::RoadEndpoint,
                                  .road = id},
                       .dist = distance(cursor, end_pos),
                   },
                   options.radius);
        }

        if (options.tangent) {
          const double ux = std::cos(away);
          const double uy = std::sin(away);
          // Cursor along the continuation ray; behind the end the endpoint
          // (or grid) snap is the meaningful answer, so require t > 0.
          const double t = (cursor.x - pose.x) * ux + (cursor.y - pose.y) * uy;
          if (t <= 0.0) {
            continue;
          }
          const Waypoint projected{.x = pose.x + t * ux, .y = pose.y + t * uy};
          consider(best_tangent,
                   Candidate{
                       .result = {.position = projected,
                                  .heading = away,
                                  .kind = SnapKind::TangentContinuation,
                                  .road = id},
                       .dist = distance(cursor, projected),
                   },
                   options.radius);
        }
      }
    });
  }

  if (best_endpoint) {
    return best_endpoint->result;
  }
  if (best_tangent) {
    return best_tangent->result;
  }

  if (options.grid && *options.grid > 0.0) {
    const double spacing = *options.grid;
    const Waypoint snapped{.x = std::round(cursor.x / spacing) * spacing,
                           .y = std::round(cursor.y / spacing) * spacing};
    if (distance(cursor, snapped) <= options.radius) {
      return SnapResult{.position = snapped, .kind = SnapKind::Grid};
    }
  }

  return std::nullopt;
}

std::optional<SideSnap> snap_to_road_side(const RoadNetwork& network,
                                          Waypoint cursor,
                                          const SnapOptions& options,
                                          double end_margin) {
  std::optional<SideSnap> best;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (options.exclude_road && *options.exclude_road == id) {
      return;
    }
    if (road.junction.is_valid()) {
      return; // connecting-road geometry belongs to the generator
    }
    const ReferenceLine& line = road.plan_view;
    const double length = line.length();
    if (length <= 2.0 * end_margin) {
      return; // no body left between the end margins
    }

    // Coarse pass: bounded sample count keeps long roads cheap; the local
    // ternary refinement below recovers the precision the samples miss.
    const double lo = end_margin;
    const double hi = length - end_margin;
    const int samples = std::clamp(static_cast<int>((hi - lo) / 2.0), 8, 512);
    double best_s = lo;
    double best_d = std::numeric_limits<double>::max();
    for (int i = 0; i <= samples; ++i) {
      const double s = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(samples);
      const PathPoint pose = line.evaluate(s);
      const double d = std::hypot(cursor.x - pose.x, cursor.y - pose.y);
      if (d < best_d) {
        best_d = d;
        best_s = s;
      }
    }
    // Ternary refinement around the best sample — distance(s) is locally
    // unimodal at interactive scales.
    const double step = (hi - lo) / static_cast<double>(samples);
    double a = std::max(lo, best_s - step);
    double b = std::min(hi, best_s + step);
    for (int i = 0; i < 40 && (b - a) > 1e-6; ++i) {
      const double m1 = a + (b - a) / 3.0;
      const double m2 = b - (b - a) / 3.0;
      const PathPoint p1 = line.evaluate(m1);
      const PathPoint p2 = line.evaluate(m2);
      const double d1 = std::hypot(cursor.x - p1.x, cursor.y - p1.y);
      const double d2 = std::hypot(cursor.x - p2.x, cursor.y - p2.y);
      if (d1 < d2) {
        b = m2;
      } else {
        a = m1;
      }
    }
    const double s = (a + b) / 2.0;
    const PathPoint pose = line.evaluate(s);
    const double d = std::hypot(cursor.x - pose.x, cursor.y - pose.y);
    if (d > options.radius) {
      return;
    }
    if (!best || d < best->distance) {
      best = SideSnap{
          .road = id, .s = s, .position = Waypoint{.x = pose.x, .y = pose.y}, .distance = d};
    }
  });
  return best;
}

} // namespace roadmaker::edit
