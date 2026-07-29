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

#include "synthetic_district.hpp"

#include <fmt/format.h>

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker::scale {
namespace {

// Amsterdam, matching the committed fixtures. Metres-per-degree at this
// latitude, near enough for a generator whose output is measured for TIME
// rather than for position.
constexpr double kLat0 = 52.3702;
constexpr double kLon0 = 4.8952;
constexpr double kMetresPerDegreeLat = 111'320.0;
constexpr double kMetresPerDegreeLon = 68'000.0;

const char* highway_for(int index, const DistrictSpec& spec) {
  if (index % spec.arterial_every == 0) {
    return "secondary";
  }
  if (index % spec.collector_every == 0) {
    return "tertiary";
  }
  return "residential";
}

} // namespace

DistrictSpec spec_from_args(int argc, char** argv) {
  DistrictSpec spec;
  constexpr std::string_view kFlag = "--blocks=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (!arg.starts_with(kFlag)) {
      continue;
    }
    const std::string_view value = arg.substr(kFlag.size());
    int parsed = 0;
    const auto [stop, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error == std::errc{} && stop == value.data() + value.size() && parsed >= 3) {
      spec.blocks = parsed;
    }
  }
  return spec;
}

std::string synthetic_district_osm(const DistrictSpec& spec) {
  const double dlat = spec.block_spacing_m / kMetresPerDegreeLat;
  const double dlon = spec.block_spacing_m / kMetresPerDegreeLon;

  std::string out;
  // Roughly 90 bytes per node plus 40 per way reference; reserving keeps the
  // generator itself off the measured path.
  out.reserve(static_cast<std::size_t>(spec.blocks) * static_cast<std::size_t>(spec.blocks) * 160);

  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<osm version=\"0.6\" generator=\"RoadMaker synthetic_district\">\n";
  out += fmt::format("  <bounds minlat=\"{:.7f}\" minlon=\"{:.7f}\" maxlat=\"{:.7f}\" "
                     "maxlon=\"{:.7f}\"/>\n",
                     kLat0,
                     kLon0,
                     kLat0 + ((spec.blocks - 1) * dlat),
                     kLon0 + ((spec.blocks - 1) * dlon));

  // Nodes on a lattice: id = 1 + (row * blocks) + column, so a way can name
  // its refs arithmetically and every crossing is a genuinely SHARED node —
  // which is what makes this exercise the topology pass rather than a
  // thousand disconnected roads.
  const auto node_id = [&spec](int row, int column) { return 1 + (row * spec.blocks) + column; };
  for (int row = 0; row < spec.blocks; ++row) {
    for (int column = 0; column < spec.blocks; ++column) {
      out += fmt::format("  <node id=\"{}\" lat=\"{:.7f}\" lon=\"{:.7f}\"/>\n",
                         node_id(row, column),
                         kLat0 + (row * dlat),
                         kLon0 + (column * dlon));
    }
  }

  int way_id = 1'000'000;
  const auto emit_way = [&out, &way_id](const std::vector<int>& refs,
                                        const char* highway,
                                        int index,
                                        const char* orientation) {
    out += fmt::format("  <way id=\"{}\">\n", ++way_id);
    for (const int ref : refs) {
      out += fmt::format("    <nd ref=\"{}\"/>\n", ref);
    }
    out += fmt::format("    <tag k=\"highway\" v=\"{}\"/>\n", highway);
    out += fmt::format("    <tag k=\"name\" v=\"{} {}\"/>\n", orientation, index);
    // A speed on the arterials only, so the <type>/<speed> path is measured
    // without every road paying for it.
    if (std::string_view(highway) == "secondary") {
      out += "    <tag k=\"maxspeed\" v=\"50\"/>\n";
    }
    out += "  </way>\n";
  };

  std::vector<int> refs;
  refs.reserve(static_cast<std::size_t>(spec.blocks));
  for (int row = 0; row < spec.blocks; ++row) {
    refs.clear();
    for (int column = 0; column < spec.blocks; ++column) {
      refs.push_back(node_id(row, column));
    }
    emit_way(refs, highway_for(row, spec), row, "Street");
  }
  for (int column = 0; column < spec.blocks; ++column) {
    refs.clear();
    for (int row = 0; row < spec.blocks; ++row) {
      refs.push_back(node_id(row, column));
    }
    emit_way(refs, highway_for(column, spec), column, "Avenue");
  }

  out += "</osm>\n";
  return out;
}

} // namespace roadmaker::scale
