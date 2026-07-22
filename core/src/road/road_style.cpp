// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/road/road_style.hpp"

namespace roadmaker {

namespace {

RoadMark broken_white() {
  return RoadMark{.type = RoadMarkType::Broken, .color = RoadMarkColor::White};
}

RoadMark solid_white() {
  return RoadMark{.type = RoadMarkType::Solid, .color = RoadMarkColor::White};
}

StyleLane driving(double width, std::optional<RoadMark> mark) {
  return StyleLane{
      .type = LaneType::Driving, .width = Poly3{.a = width}, .outer_mark = std::move(mark)};
}

StyleLane plain(LaneType type, double width) {
  return StyleLane{.type = type, .width = Poly3{.a = width}, .outer_mark = std::nullopt};
}

} // namespace

RoadStyle RoadStyle::urban_two_lane() {
  // Inner lane's outer boundary is the divider to the outer same-direction
  // lane: a dashed white line (#194). The outer lane's outer boundary is the
  // road edge: a solid white line.
  return RoadStyle{
      .left = {driving(3.5, broken_white()), driving(3.5, solid_white())},
      .right = {driving(3.5, broken_white()), driving(3.5, solid_white())},
      .center_mark = RoadMark{.type = RoadMarkType::Solid, .color = RoadMarkColor::Yellow},
  };
}

RoadStyle RoadStyle::two_lane_rural() {
  return RoadStyle{
      .left = {driving(3.5, solid_white())},
      .right = {driving(3.5, solid_white()), plain(LaneType::Shoulder, 1.0)},
      .center_mark = broken_white(),
  };
}

RoadStyle RoadStyle::highway() {
  return RoadStyle{
      .left = {driving(3.75, broken_white()),
               driving(3.75, solid_white()),
               plain(LaneType::Shoulder, 2.5)},
      .right = {driving(3.75, broken_white()),
                driving(3.75, solid_white()),
                plain(LaneType::Shoulder, 2.5)},
      .center_mark = std::nullopt,
  };
}

} // namespace roadmaker
