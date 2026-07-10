#include "roadmaker/edit/snap.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <cmath>
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

        if (options.endpoints) {
          consider(best_endpoint,
                   Candidate{
                       .result = {.position = end_pos, .kind = SnapKind::RoadEndpoint, .road = id},
                       .dist = distance(cursor, end_pos),
                   },
                   options.radius);
        }

        if (options.tangent) {
          const double away = at_end ? pose.hdg : wrap_angle(pose.hdg + std::numbers::pi);
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

} // namespace roadmaker::edit
