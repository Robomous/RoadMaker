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

#include "roadmaker/road/defaults.hpp"

namespace roadmaker {

namespace {

RoadMark broken_white() {
  return RoadMark{.type = RoadMarkType::Broken, .color = RoadMarkColor::White};
}

RoadMark solid_white() {
  return RoadMark{.type = RoadMarkType::Solid, .color = RoadMarkColor::White};
}

RoadMark broken_yellow() {
  return RoadMark{.type = RoadMarkType::Broken, .color = RoadMarkColor::Yellow};
}

RoadMark solid_yellow() {
  return RoadMark{.type = RoadMarkType::Solid, .color = RoadMarkColor::Yellow};
}

StyleLane driving(double width, std::optional<RoadMark> mark) {
  return StyleLane{
      .type = LaneType::Driving, .width = Poly3{.a = width}, .outer_mark = std::move(mark)};
}

StyleLane plain(LaneType type, double width) {
  return StyleLane{.type = type, .width = Poly3{.a = width}, .outer_mark = std::nullopt};
}

StyleLane shoulder_with_mark(double width, RoadMark mark) {
  return StyleLane{
      .type = LaneType::Shoulder, .width = Poly3{.a = width}, .outer_mark = std::move(mark)};
}

} // namespace

RoadStyle RoadStyle::freeway() {
  // Inner lane's outer boundary is the divider to the outer same-direction
  // lane: a dashed white line (#194). The outer lane's outer boundary is the
  // road edge: a solid white line. The innermost shoulder's outer boundary is
  // the left edge of the traveled way — yellow on a divided road (§1.3).
  const double lane = defaults::driving_lane_width(defaults::RoadClass::Freeway);
  const auto side = [lane] {
    return std::vector<StyleLane>{
        shoulder_with_mark(defaults::kFreewayLeftShoulderWidth, solid_yellow()),
        driving(lane, broken_white()),
        driving(lane, solid_white()),
        plain(LaneType::Shoulder, defaults::kFreewayRightShoulderWidth)};
  };
  return RoadStyle{.left = side(), .right = side(), .center_mark = std::nullopt};
}

RoadStyle RoadStyle::arterial() {
  const double lane = defaults::driving_lane_width(defaults::RoadClass::Arterial);
  const auto side = [lane] {
    return std::vector<StyleLane>{driving(lane, broken_white()),
                                  driving(lane, solid_white()),
                                  plain(LaneType::Sidewalk, defaults::kSidewalkWidth)};
  };
  return RoadStyle{
      .left = side(),
      .right = side(),
      .center_mark = RoadMark{.type = RoadMarkType::SolidSolid, .color = RoadMarkColor::Yellow},
  };
}

RoadStyle RoadStyle::collector() {
  const double lane = defaults::driving_lane_width(defaults::RoadClass::Collector);
  return RoadStyle{
      .left = {driving(lane, solid_white())},
      .right = {driving(lane, solid_white()), plain(LaneType::Shoulder, defaults::kShoulderWidth)},
      .center_mark = broken_yellow(),
  };
}

RoadStyle RoadStyle::local_road() {
  const double lane = defaults::driving_lane_width(defaults::RoadClass::Local);
  const auto side = [lane] {
    return std::vector<StyleLane>{driving(lane, std::nullopt),
                                  plain(LaneType::Sidewalk, defaults::kSidewalkWidth)};
  };
  return RoadStyle{.left = side(), .right = side(), .center_mark = std::nullopt};
}

RoadStyle RoadStyle::urban_two_lane() {
  return arterial();
}

RoadStyle RoadStyle::two_lane_rural() {
  return collector();
}

RoadStyle RoadStyle::highway() {
  return freeway();
}

} // namespace roadmaker
