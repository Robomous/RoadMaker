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

#include "roadmaker/road/defaults.hpp"

#include "roadmaker/road/lane.hpp"

#include <fmt/format.h>

namespace roadmaker::defaults {

namespace {

/// The doc's number style: sub-metre values keep two decimals ("0.10",
/// "0.60"), metre-scale values keep one ("3.6", "3.0"). The renderers below
/// are the source of the committed tables, so this policy IS the doc policy.
std::string len(double meters) {
  return meters < 1.0 ? fmt::format("{:.2f}", meters) : fmt::format("{:.1f}", meters);
}

} // namespace

double driving_lane_width(RoadClass road_class) {
  switch (road_class) {
  case RoadClass::Freeway:
    return kFreewayLaneWidth;
  case RoadClass::Arterial:
    return kArterialLaneWidth;
  case RoadClass::Collector:
    return kCollectorLaneWidth;
  case RoadClass::Local:
    return kLocalLaneWidth;
  }
  return kArterialLaneWidth;
}

double lane_width(LaneType type) {
  switch (type) {
  case LaneType::Shoulder:
    return kShoulderWidth;
  case LaneType::Sidewalk:
    return kSidewalkWidth;
  case LaneType::Biking:
    return kBikeLaneWidth;
  case LaneType::Parking:
    return kParkingLaneWidth;
  case LaneType::Median:
    return kMedianWidth;
  default:
    return kArterialLaneWidth;
  }
}

const char* road_class_name(RoadClass road_class) {
  switch (road_class) {
  case RoadClass::Freeway:
    return "freeway";
  case RoadClass::Arterial:
    return "arterial";
  case RoadClass::Collector:
    return "collector";
  case RoadClass::Local:
    return "local";
  }
  return "arterial";
}

std::string cross_section_markdown() {
  std::string out = "<!-- rm-defaults: cross-section -->\n"
                    "| Element | Default | Imperial display | Range | Basis |\n"
                    "|---|---|---|---|---|\n";
  const auto row = [&out](const char* element,
                          const std::string& value,
                          const char* imperial,
                          const char* range,
                          const char* basis) {
    out += fmt::format("| {} | {} | {} | {} | {} |\n", element, value, imperial, range, basis);
  };
  const auto bold = [](double meters) { return fmt::format("**{} m**", len(meters)); };
  row("Freeway/highway lane", bold(kFreewayLaneWidth), "12 ft", "3.6", "AASHTO");
  row("Arterial lane", bold(kArterialLaneWidth), "12 ft", "3.3–3.6", "AASHTO");
  row("Collector lane", bold(kCollectorLaneWidth), "11 ft", "3.0–3.6", "AASHTO");
  row("Local/residential lane", bold(kLocalLaneWidth), "10 ft", "2.7–3.6", "AASHTO");
  row("Freeway right shoulder", bold(kFreewayRightShoulderWidth), "10 ft", "—", "AASHTO");
  row("Freeway left shoulder", bold(kFreewayLeftShoulderWidth), "4 ft", "—", "AASHTO");
  row("Arterial/collector shoulder", bold(kShoulderWidth), "6 ft", "0.6–2.4", "AASHTO");
  row("Parking lane", bold(kParkingLaneWidth), "8 ft", "2.4–2.7", "typical NA");
  row("Bike lane", bold(kBikeLaneWidth), "5 ft", "1.5–1.8", "AASHTO/NACTO");
  row("Sidewalk", bold(kSidewalkWidth), "6 ft", "1.5 min", "ADA/typical");
  row("Curb height", bold(kCurbHeight), "6 in", "0.10–0.20", "typical NA");
  row("Raised median", bold(kMedianWidth) + " min", "4 ft", "—", "AASHTO");
  row("Two-way left-turn lane", bold(kTwoWayLeftTurnLaneWidth), "12 ft", "3.0–4.2", "AASHTO");
  return out;
}

std::string markings_markdown() {
  std::string out = "<!-- rm-defaults: markings -->\n"
                    "| Item | Default | Imperial display |\n"
                    "|---|---|---|\n";
  const auto row = [&out](const char* item, const std::string& value, const char* imperial) {
    out += fmt::format("| {} | {} | {} |\n", item, value, imperial);
  };
  row("Normal line width",
      fmt::format("**{} m**; freeway option {} m", len(kLineWidth), len(kFreewayLineWidth)),
      "4 in; 6 in");
  row("Broken lane line",
      fmt::format("**{} m dash / {} m gap**", len(kDashLength), len(kDashGap)),
      "10 ft / 30 ft");
  row("Double yellow centerline",
      fmt::format("two normal lines, **{} m** apart", len(kDoubleLineSeparation)),
      "4 in");
  row("Stop line",
      fmt::format("**{} m** wide; min {} m", len(kStopLineWidth), len(kStopLineMinWidth)),
      "24 in; 12 in");
  row("Crosswalk transverse lines",
      fmt::format("{}–{} m wide", len(kCrosswalkLineMinWidth), len(kCrosswalkLineMaxWidth)),
      "6–24 in");
  row("Crosswalk zebra stripe",
      fmt::format("**{} m bar / {} m gap**", len(kCrosswalkStripeLength), len(kCrosswalkStripeGap)),
      "24 in / 24 in");
  row("Crosswalk width",
      fmt::format(
          "**{} m** walking depth; min {} m", len(kCrosswalkWidth), len(kCrosswalkMinWidth)),
      "10 ft; 6 ft");
  row("Edge lines", "white right edge; yellow left edge (divided)", "—");
  return out;
}

} // namespace roadmaker::defaults
