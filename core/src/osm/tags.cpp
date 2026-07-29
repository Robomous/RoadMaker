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

#include "roadmaker/osm/tags.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <utility>

namespace roadmaker::osm {
namespace {

using defaults::RoadClass;

constexpr std::array<HighwayMapping, 15> kHighways{{
    {.value = "motorway", .road_class = RoadClass::Freeway, .link = false, .implies_oneway = true},
    {.value = "motorway_link",
     .road_class = RoadClass::Freeway,
     .link = true,
     .implies_oneway = true},
    {.value = "trunk", .road_class = RoadClass::Freeway, .link = false, .implies_oneway = false},
    {.value = "trunk_link", .road_class = RoadClass::Freeway, .link = true, .implies_oneway = true},
    {.value = "primary", .road_class = RoadClass::Arterial, .link = false, .implies_oneway = false},
    {.value = "primary_link",
     .road_class = RoadClass::Arterial,
     .link = true,
     .implies_oneway = true},
    {.value = "secondary",
     .road_class = RoadClass::Arterial,
     .link = false,
     .implies_oneway = false},
    {.value = "secondary_link",
     .road_class = RoadClass::Arterial,
     .link = true,
     .implies_oneway = true},
    {.value = "tertiary",
     .road_class = RoadClass::Collector,
     .link = false,
     .implies_oneway = false},
    {.value = "tertiary_link",
     .road_class = RoadClass::Collector,
     .link = true,
     .implies_oneway = true},
    {.value = "unclassified",
     .road_class = RoadClass::Collector,
     .link = false,
     .implies_oneway = false},
    {.value = "residential",
     .road_class = RoadClass::Local,
     .link = false,
     .implies_oneway = false},
    {.value = "living_street",
     .road_class = RoadClass::Local,
     .link = false,
     .implies_oneway = false},
    {.value = "road", .road_class = RoadClass::Collector, .link = false, .implies_oneway = false},
    {.value = "service", .road_class = RoadClass::Local, .link = false, .implies_oneway = false},
}};

/// Recognised NON-road classifications. Listed rather than left to fall through
/// the unknown path so the diagnostic can say "not a road" instead of
/// "unrecognised" — different facts, and only one of them is the user's problem.
constexpr std::array<std::string_view, 22> kDropped{{
    "footway",   "path",         "cycleway",      "pedestrian",     "steps",        "track",
    "bridleway", "corridor",     "platform",      "busway",         "bus_guideway", "escape",
    "raceway",   "construction", "proposed",      "rest_area",      "services",     "bus_stop",
    "crossing",  "elevator",     "emergency_bay", "traffic_island",
}};

constexpr std::array<TagMapping, 18> kTags{{
    {.key = "name", .use = TagUse::Modeled, .effect = "road name"},
    {.key = "oneway",
     .use = TagUse::Modeled,
     .effect = "one-way cross section; `-1` reverses the polyline"},
    {.key = "lanes", .use = TagUse::Modeled, .effect = "driving lanes, split across directions"},
    {.key = "lanes:forward",
     .use = TagUse::Modeled,
     .effect = "driving lanes in the reference-line direction"},
    {.key = "lanes:backward",
     .use = TagUse::Modeled,
     .effect = "driving lanes against the reference-line direction"},
    {.key = "maxspeed", .use = TagUse::Modeled, .effect = "`<type><speed @max @unit>`"},
    {.key = "junction", .use = TagUse::Modeled, .effect = "`roundabout`/`circular` imply oneway"},
    {.key = "layer", .use = TagUse::Modeled, .effect = "topology only — see §4"},
    {.key = "bridge", .use = TagUse::Modeled, .effect = "reported, imported at grade"},
    {.key = "tunnel", .use = TagUse::Modeled, .effect = "reported, imported at grade"},
    {.key = "surface", .use = TagUse::Dropped, .effect = ""},
    {.key = "smoothness", .use = TagUse::Dropped, .effect = ""},
    {.key = "width", .use = TagUse::Dropped, .effect = ""},
    {.key = "sidewalk", .use = TagUse::Dropped, .effect = ""},
    {.key = "turn:lanes", .use = TagUse::Dropped, .effect = ""},
    {.key = "cycleway", .use = TagUse::Dropped, .effect = ""},
    {.key = "lit", .use = TagUse::Dropped, .effect = ""},
    {.key = "ref", .use = TagUse::Dropped, .effect = ""},
}};

} // namespace

std::span<const HighwayMapping> highway_mappings() {
  return kHighways;
}

const HighwayMapping* highway_mapping(std::string_view value) {
  const auto found = std::ranges::find(kHighways, value, &HighwayMapping::value);
  return found == kHighways.end() ? nullptr : &*found;
}

std::span<const std::string_view> dropped_highway_values() {
  return kDropped;
}

bool is_dropped_highway(std::string_view value) {
  return std::ranges::find(kDropped, value) != kDropped.end();
}

std::span<const TagMapping> tag_mappings() {
  return kTags;
}

OneWay parse_oneway(std::string_view value) {
  if (value == "-1" || value == "reverse") {
    return {.one_way = true, .reversed = true};
  }
  if (value == "yes" || value == "true" || value == "1") {
    return {.one_way = true, .reversed = false};
  }
  return {.one_way = false, .reversed = false};
}

std::optional<int> parse_lane_count(std::string_view value) {
  int lanes = 0;
  const char* const begin = value.data();
  const char* const end = begin + value.size();
  const auto [stop, error] = std::from_chars(begin, end, lanes);
  // The WHOLE value must be the number: "1;2" and "2 lanes" are OSM's way of
  // saying "it is complicated", and reading the leading 2 would be inventing a
  // certainty the source declined to state.
  if (error != std::errc{} || stop != end || lanes <= 0) {
    return std::nullopt;
  }
  return std::min(lanes, kMaxLanesPerSide * 2);
}

std::optional<MaxSpeed> parse_maxspeed(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  // The autobahn case. §10.4.1's t_maxSpeed literal, carried as a string
  // precisely because it has no numeric form.
  if (value == "none") {
    return MaxSpeed{.max = "no limit", .unit = ""};
  }

  std::string_view number = value;
  std::string unit = "km/h"; // OSM's convention: a bare number is km/h.
  if (const std::size_t space = value.rfind(' '); space != std::string_view::npos) {
    const std::string_view suffix = value.substr(space + 1);
    if (suffix == "mph") {
      // mph deliberately bypasses the units layer — the settled policy. The
      // unit travels with the value and nothing converts it.
      unit = "mph";
    } else if (suffix == "km/h" || suffix == "kmh" || suffix == "kph") {
      unit = "km/h";
    } else {
      return std::nullopt; // "walk", "signals", "variable", anything else
    }
    number = value.substr(0, space);
  }

  double parsed = 0.0;
  const char* const begin = number.data();
  const char* const end = begin + number.size();
  const auto [stop, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc{} || stop != end || !(parsed > 0.0)) {
    return std::nullopt;
  }
  return MaxSpeed{.max = std::string(number), .unit = std::move(unit)};
}

int parse_layer(std::string_view value) {
  int layer = 0;
  const char* const begin = value.data();
  const char* const end = begin + value.size();
  const auto [stop, error] = std::from_chars(begin, end, layer);
  if (error != std::errc{} || stop != end) {
    return 0;
  }
  return layer;
}

std::string highway_mapping_markdown() {
  std::string out = "<!-- rm-osm: highway -->\n"
                    "| `highway=` | Imported as | Link ramp | Implies oneway |\n"
                    "|---|---|---|---|\n";
  for (const HighwayMapping& row : kHighways) {
    out += fmt::format("| `{}` | {} | {} | {} |\n",
                       row.value,
                       row.road_class ? defaults::road_class_name(*row.road_class) : "—",
                       row.link ? "yes" : "no",
                       row.implies_oneway ? "yes" : "no");
  }
  return out;
}

std::string tag_mapping_markdown() {
  std::string out = "<!-- rm-osm: tags -->\n"
                    "| Tag | Effect | Dropped |\n"
                    "|---|---|---|\n";
  for (const TagMapping& row : kTags) {
    out += fmt::format("| `{}` | {} | {} |\n",
                       row.key,
                       row.use == TagUse::Modeled ? row.effect : std::string_view("—"),
                       row.use == TagUse::Modeled ? "no" : "yes");
  }
  return out;
}

} // namespace roadmaker::osm
