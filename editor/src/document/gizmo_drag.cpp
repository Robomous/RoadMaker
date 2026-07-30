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

#include "document/gizmo_drag.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/signal.hpp"

#include <QObject>
#include <cmath>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/prop_placement.hpp"
#include "document/signal_placement.hpp" // kSignalSnapThreshold
#include "document/transform_gates.hpp"
#include "document/units.hpp"
#include "viewport/picking.hpp" // station_to_world, station_within, kObjectSnapThreshold

namespace roadmaker::editor {

namespace {

/// The detent for this frame: the registry's increment, or 0 (free) while the
/// suppression modifier is held. snap_to_increment treats 0 as a passthrough,
/// so the two cases share one code path.
double rotation_increment(bool free_rotation) {
  return free_rotation ? 0.0 : defaults::kPropRotationSnap;
}

double degrees(double radians) {
  return radians * 180.0 / std::numbers::pi;
}

} // namespace

std::optional<GizmoTarget> gizmo_target(const RoadNetwork& network,
                                        const SelectionEntry& entry,
                                        std::span<const RoadId> selected_roads) {
  // Leaf entities first — see the header: both carry their owning road, so the
  // road test must come last or a sign resolves to the road under it.
  if (entry.object.is_valid()) {
    const Object* object = network.object(entry.object);
    if (object == nullptr) {
      return std::nullopt;
    }
    const Road* road = network.road(object->road);
    if (road == nullptr || road->plan_view.empty()) {
      return std::nullopt;
    }
    const auto p = station_to_world(road->plan_view, object->s, object->t);
    return GizmoTarget{.object = entry.object, .pivot = {p[0], p[1], 0.0}};
  }
  if (entry.signal.is_valid()) {
    const Signal* signal = network.signal(entry.signal);
    if (signal == nullptr) {
      return std::nullopt;
    }
    const Road* road = network.road(signal->road);
    if (road == nullptr || road->plan_view.empty()) {
      return std::nullopt;
    }
    const auto p = station_to_world(road->plan_view, signal->s, signal->t);
    return GizmoTarget{.signal = entry.signal, .pivot = {p[0], p[1], 0.0}};
  }
  if (entry.road.is_valid()) {
    const Road* road = network.road(entry.road);
    if (road == nullptr || road->plan_view.empty()) {
      return std::nullopt;
    }
    const PathPoint pose = road->plan_view.evaluate(road->plan_view.length() / 2.0);
    // The gizmo stays where it is — on this road, at its midpoint — but it
    // carries the whole road selection when this road is part of one. The pivot
    // is deliberately NOT re-centred on the selection: the user grabbed a ring
    // drawn here, and moving it under the press would be its own surprise.
    std::vector<RoadId> roads{entry.road};
    if (std::ranges::find(selected_roads, entry.road) != selected_roads.end()) {
      for (const RoadId id : selected_roads) {
        if (id != entry.road) {
          roads.push_back(id);
        }
      }
    }
    return GizmoTarget{
        .road = entry.road, .pivot = {pose.x, pose.y, 0.0}, .roads = std::move(roads)};
  }
  return std::nullopt;
}

std::optional<GizmoTarget> gizmo_target(const RoadNetwork& network, const SelectionEntry& entry) {
  return gizmo_target(network, entry, {});
}

GizmoDragSession::GizmoDragSession(Document& document) : document_(document) {}

GizmoDragStart GizmoDragSession::begin(const GizmoTarget& target,
                                       GizmoHandle handle,
                                       std::array<double, 2> press_world) {
  refusal_.clear();
  reported_refusal_.clear();
  if (handle == GizmoHandle::None) {
    return GizmoDragStart::Ignored;
  }
  // Only a road offers a Z arm (props and signals have no kernel z-move op
  // yet, and the arm is not drawn for them), so a Z grab on a leaf is a miss
  // rather than a dead drag.
  if (handle == GizmoHandle::AxisZ && !target.road.is_valid()) {
    return GizmoDragStart::Ignored;
  }
  // The one gate a ROAD transform passes: refuse what the kernel would refuse
  // at the grab. A leaf entity moves within its road, so it does not apply.
  //
  // AxisZ is deliberately ungated: edit::set_elevation_profile ACCEPTS a
  // junction arm (it dirties the junction so it regenerates), and raising an
  // arm is a supported gesture — gating it here would be a regression. Since
  // cascade-s2 (#462) the same is true of the plane and ring handles for an
  // ARM; only a connecting road is still refused, which is all the gate now
  // looks for.
  if (target.road.is_valid() && !target.object.is_valid() && !target.signal.is_valid() &&
      handle != GizmoHandle::AxisZ) {
    const TransformKind kind =
        handle == GizmoHandle::YawRing ? TransformKind::Rotate : TransformKind::Translate;
    if (std::optional<QString> refusal =
            junction_transform_refusal(document_.network(), target.roads, kind)) {
      refusal_ = *std::move(refusal);
      return GizmoDragStart::Refused;
    }
  }
  double base_angle = 0.0;
  if (target.object.is_valid()) {
    if (const Object* object = document_.network().object(target.object)) {
      base_angle = object->hdg;
    }
  } else if (target.signal.is_valid()) {
    if (const Signal* signal = document_.network().signal(target.signal)) {
      base_angle = signal->h_offset;
    }
  }
  drag_ = Drag{.handle = handle,
               .target = target,
               .press_world = press_world,
               .base_angle = base_angle,
               .summary = QString()};
  return GizmoDragStart::Armed;
}

Expected<void> GizmoDragSession::update(const GizmoDragInput& input) {
  if (!drag_.has_value()) {
    return {};
  }
  Drag& drag = *drag_;
  const std::array<double, 2> pivot_xy{drag.target.pivot[0], drag.target.pivot[1]};

  // Every factory runs against the BASE-state network (the session reverts to
  // it each frame), so all deltas are absolute from the press.
  Document::PreviewFactory factory;

  if (drag.handle == GizmoHandle::YawRing) {
    const double increment = rotation_increment(input.free_rotation);
    const double raw = gizmo_yaw_angle(pivot_xy, drag.press_world, input.cursor_world);
    if (drag.target.object.is_valid()) {
      // Absolute detents: snap the RESULTING road-relative heading, not the
      // delta, so the prop lands on an exact multiple of the increment. hdg is
      // road-relative already (OpenDRIVE §11), and the road tangent under the
      // prop is fixed for the drag, so a world yaw delta IS an hdg delta.
      const ObjectId object = drag.target.object;
      const double hdg = wrap_angle(snap_to_increment(drag.base_angle + raw, increment));
      factory = [object, hdg](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
        const Object* o = base.object(object);
        if (o == nullptr) {
          return {};
        }
        return move_object_command(base, object, o->s, o->t, hdg);
      };
      drag.summary = QObject::tr("Rotated prop to %1°").arg(degrees(hdg), 0, 'f', 0);
    } else if (drag.target.signal.is_valid()) {
      // @hOffset only — @orientation says which traffic the sign APPLIES to
      // (§14.1) and a rotation gesture must not rewrite that. The facing datum
      // is fixed by @orientation, so a world yaw delta is an @hOffset delta.
      const SignalId signal_id = drag.target.signal;
      const double h_offset = wrap_angle(snap_to_increment(drag.base_angle + raw, increment));
      factory = [signal_id, h_offset](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
        const Signal* s = base.signal(signal_id);
        if (s == nullptr) {
          return {};
        }
        return edit::move_signal(base, signal_id, s->s, s->t, h_offset);
      };
      drag.summary = QObject::tr("Rotated sign to %1°").arg(degrees(h_offset), 0, 'f', 0);
    } else {
      // A road is rotated BY a delta about a pivot, so the delta is what snaps.
      // The whole road selection turns, which is what makes a rigid
      // whole-junction rotation reachable: select every arm, grab the ring.
      const std::vector<RoadId> roads = drag.target.roads;
      const double angle = snap_to_increment(raw, increment);
      factory = [roads, pivot_xy, angle](const RoadNetwork& base) {
        return edit::rotate_roads(
            base, roads, angle, pivot_xy[0], pivot_xy[1], edit::TurnSetPolicy::InPlaceOnly);
      };
      const Road* r = document_.network().road(drag.target.road);
      drag.summary = roads.size() > 1
                         ? QObject::tr("Rotated %1 roads by %2°")
                               .arg(roads.size())
                               .arg(degrees(angle), 0, 'f', 0)
                         : QObject::tr("Rotated road %1 by %2°")
                               .arg(r != nullptr ? QString::fromStdString(r->odr_id) : QString())
                               .arg(degrees(angle), 0, 'f', 0);
    }
  } else if (drag.handle == GizmoHandle::AxisZ) {
    const RoadId road = drag.target.road;
    const double dz = input.dz;
    factory = [road, dz](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
      const Road* r = base.road(road);
      if (r == nullptr) {
        return {};
      }
      std::vector<edit::ElevationPoint> points = edit::elevation_profile_points(*r);
      if (points.empty()) {
        points = {edit::ElevationPoint{.s = 0.0, .z = dz},
                  edit::ElevationPoint{.s = r->plan_view.length(), .z = dz}};
      } else {
        for (edit::ElevationPoint& point : points) {
          point.z += dz;
        }
      }
      return edit::set_elevation_profile(base, road, std::move(points));
    };
    drag.summary = QObject::tr("Raised road by %1").arg(units::format_length(dz));
  } else {
    // Planar translate (AxisX / AxisY / PlaneXY).
    const auto delta =
        gizmo_constrain_translation(drag.handle, drag.press_world, input.cursor_world);
    const double nx = drag.target.pivot[0] + delta[0];
    const double ny = drag.target.pivot[1] + delta[1];
    if (drag.target.object.is_valid()) {
      const ObjectId object = drag.target.object;
      factory = [object, nx, ny](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
        const Object* o = base.object(object);
        const Road* r = o != nullptr ? base.road(o->road) : nullptr;
        if (r == nullptr) {
          return {};
        }
        // Same road-relative guard as the prop move-drag: a gizmo dragged clear
        // of the road yields no command, so update_preview keeps the last good
        // frame rather than flinging the prop out to a huge t.
        const std::optional<StationCoord> station =
            station_within(r->plan_view, nx, ny, kObjectSnapThreshold);
        if (!station.has_value()) {
          return {};
        }
        return move_object_command(base, object, station->s, station->t);
      };
      drag.summary = QObject::tr("Moved prop");
    } else if (drag.target.signal.is_valid()) {
      const SignalId signal_id = drag.target.signal;
      factory = [signal_id, nx, ny](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
        const Signal* s = base.signal(signal_id);
        const Road* r = s != nullptr ? base.road(s->road) : nullptr;
        if (r == nullptr) {
          return {};
        }
        const std::optional<StationCoord> station =
            station_within(r->plan_view, nx, ny, kSignalSnapThreshold);
        if (!station.has_value()) {
          return {};
        }
        // std::nullopt: translating a sign leaves its heading exactly as
        // authored — only the ring and the explicit Auto facing action touch it.
        return edit::move_signal(base, signal_id, station->s, station->t, std::nullopt);
      };
      drag.summary = QObject::tr("Moved sign");
    } else {
      const std::vector<RoadId> roads = drag.target.roads;
      const double dx = delta[0];
      const double dy = delta[1];
      factory = [roads, dx, dy](const RoadNetwork& base) {
        return edit::translate_roads(base, roads, dx, dy, edit::TurnSetPolicy::InPlaceOnly);
      };
      drag.summary = roads.size() > 1 ? QObject::tr("Moved %1 roads").arg(roads.size())
                                      : QObject::tr("Moved road");
    }
  }

  // A factory that yields NO command is a frame with nothing to edit (a prop
  // dragged clear of its road), not a failure. Document reports both of those
  // as errors ("null command" / "preview factory returned no command"), so the
  // null has to be absorbed HERE — otherwise every such frame hands the caller
  // an internal error to surface, and dragging a prop off its road becomes a
  // wall of toasts (#401).
  // A refused frame is not a broken editor: dragging a junction arm out of its
  // junction's reach is a supported gesture that the kernel declines, and the
  // preview simply stops following. Stash the reason for ONE toast — returning
  // it would put a toast on every mouse-move, which is #401's own bug. This
  // applies to the FIRST frame as much as the rest: a drag can be refused before
  // any preview exists.
  const auto absorb = [this](const Expected<void>& result) -> Expected<void> {
    if (result.has_value()) {
      return result;
    }
    const QString reason =
        QObject::tr("Cannot transform: %1").arg(QString::fromStdString(result.error().message));
    // Once per REASON, not once per frame: a drag held past the refusal point
    // repeats the same message sixty times a second, and re-stashing it after
    // each take() would put every one of them on screen. A genuinely different
    // refusal later in the same drag still gets said.
    if (reason != reported_refusal_) {
      refusal_ = reason;
      reported_refusal_ = reason;
    }
    return {};
  };

  if (!document_.preview_active()) {
    std::unique_ptr<edit::Command> command = factory(document_.network());
    if (command == nullptr) {
      return {};
    }
    return absorb(document_.begin_preview(std::move(command)));
  }
  bool produced = false;
  const Expected<void> result =
      document_.update_preview([&factory, &produced](const RoadNetwork& base) {
        std::unique_ptr<edit::Command> command = factory(base);
        produced = command != nullptr;
        return command;
      });
  // update_preview has already restored the last good preview, so a frame that
  // produced nothing leaves the session exactly where it was.
  return produced ? absorb(result) : Expected<void>{};
}

std::optional<QString> GizmoDragSession::take_refusal() {
  if (refusal_.isEmpty()) {
    return std::nullopt;
  }
  return std::exchange(refusal_, QString());
}

bool GizmoDragSession::commit() {
  if (!drag_.has_value()) {
    return false;
  }
  const bool had_preview = document_.preview_active();
  document_.commit_preview();
  drag_.reset();
  return had_preview;
}

void GizmoDragSession::cancel() {
  if (!drag_.has_value()) {
    return;
  }
  document_.cancel_preview();
  drag_.reset();
}

GizmoHandle GizmoDragSession::handle() const {
  return drag_.has_value() ? drag_->handle : GizmoHandle::None;
}

QString GizmoDragSession::summary() const {
  return drag_.has_value() ? drag_->summary : QString();
}

} // namespace roadmaker::editor
