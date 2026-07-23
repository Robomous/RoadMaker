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
