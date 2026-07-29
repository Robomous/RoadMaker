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

#include "roadmaker/osm/graph.hpp"
#include "roadmaker/osm/tags.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace roadmaker::osm {
namespace {

/// The refusal ADR-0012 records, worded so a user can act on it. It names the
/// dependency, the workaround and the issue — the contract every refusal
/// diagnostic in this codebase carries since ADR-0010.
constexpr std::string_view kPbfRefusal =
    "'{}' is an OSM Protocol Buffers file. This build reads the .osm XML "
    "interchange format only: a .pbf stores its blobs zlib-deflated and zlib "
    "is not a RoadMaker dependency (ADR-0012; it arrives with #484, and the "
    "reader is tracked as #494). Convert it with 'osmium cat -o district.osm "
    "district.osm.pbf', or query the Overpass API, which returns .osm XML.";

bool ends_with_ci(const std::string& text, std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

/// pugixml's `as_double` returns 0.0 for a malformed value, which is a real
/// coordinate off the west coast of Africa. Every node needs to know the
/// difference between "at zero" and "unreadable".
bool read_coordinate(const pugi::xml_node& node, const char* name, double& out) {
  const pugi::xml_attribute attr = node.attribute(name);
  if (!attr) {
    return false;
  }
  const std::string text = attr.value();
  try {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    out = value;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

Expected<OsmParseResult>
build(const pugi::xml_document& document, std::string_view source, const OsmReadOptions& options) {
  const pugi::xml_node root = document.child("osm");
  if (!root) {
    return make_error(ErrorCode::InvalidDocument,
                      "not an OpenStreetMap XML document: no <osm> root element",
                      std::string(source));
  }

  OsmParseResult result;
  OsmGraph& graph = result.graph;
  const auto diag = [&result, source](Severity severity, std::string message) {
    result.diagnostics.push_back(Diagnostic{
        .severity = severity, .location = std::string(source), .message = std::move(message)});
  };

  // --- budget, decided BEFORE a coordinate is read -------------------------
  // The #243 discipline: a file too large to hold is too large to read and
  // then discard, and refusing after the work is done is the worst of both.
  const std::size_t declared_nodes = static_cast<std::size_t>(
      std::distance(root.children("node").begin(), root.children("node").end()));
  const std::size_t declared_ways = static_cast<std::size_t>(
      std::distance(root.children("way").begin(), root.children("way").end()));
  graph.source_node_count = declared_nodes;
  graph.source_way_count = declared_ways;

  if (declared_nodes > options.max_nodes) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("extract carries {} nodes, over this build's {} limit; crop the bounding "
                    "box or filter the extract before importing",
                    declared_nodes,
                    options.max_nodes),
        std::string(source));
  }
  if (declared_ways > options.max_ways) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("extract carries {} ways, over this build's {} limit; crop the bounding "
                    "box or filter the extract before importing",
                    declared_ways,
                    options.max_ways),
        std::string(source));
  }

  // --- nodes ---------------------------------------------------------------
  graph.nodes.reserve(declared_nodes);
  std::size_t unreadable_nodes = 0;
  for (const pugi::xml_node node : root.children("node")) {
    const pugi::xml_attribute id = node.attribute("id");
    double lon = 0.0;
    double lat = 0.0;
    if (!id || !read_coordinate(node, "lon", lon) || !read_coordinate(node, "lat", lat)) {
      ++unreadable_nodes;
      continue;
    }
    graph.nodes.emplace(static_cast<OsmId>(id.as_llong()), OsmNode{.lon_deg = lon, .lat_deg = lat});
  }
  if (unreadable_nodes > 0) {
    diag(Severity::Warning,
         fmt::format("{} node(s) had no usable id/lat/lon and were skipped", unreadable_nodes));
  }

  // --- ways ----------------------------------------------------------------
  std::size_t non_highway = 0;
  std::size_t dangling_refs = 0;
  for (const pugi::xml_node node : root.children("way")) {
    OsmWay way;
    way.id = static_cast<OsmId>(node.attribute("id").as_llong());
    for (const pugi::xml_node tag : node.children("tag")) {
      way.tags.emplace_back(tag.attribute("k").value(), tag.attribute("v").value());
    }
    if (!options.keep_non_highway_ways && !way.has_tag("highway")) {
      ++non_highway;
      continue;
    }
    for (const pugi::xml_node ref : node.children("nd")) {
      const OsmId id = static_cast<OsmId>(ref.attribute("ref").as_llong());
      // A node referenced but not present is normal in a cropped extract: the
      // way runs off the edge of the bounding box. Dropping the reference
      // shortens the way, which is right; dropping the way would lose a road
      // that is genuinely there.
      if (!graph.nodes.contains(id)) {
        ++dangling_refs;
        continue;
      }
      way.refs.push_back(id);
    }
    graph.ways.push_back(std::move(way));
  }
  if (non_highway > 0) {
    diag(Severity::Info,
         fmt::format("{} way(s) carry no 'highway' tag and were not read (buildings, waterways, "
                     "landuse); this importer reads road centrelines",
                     non_highway));
  }
  if (dangling_refs > 0) {
    diag(Severity::Info,
         fmt::format("{} way node reference(s) fell outside the extract and were trimmed; a way "
                     "crossing the bounding-box edge is shortened, not dropped",
                     dangling_refs));
  }

  // --- relations, counted and not used -------------------------------------
  for (const pugi::xml_node node : root.children("relation")) {
    ++graph.relation_count;
    for (const pugi::xml_node tag : node.children("tag")) {
      if (std::string_view(tag.attribute("k").value()) == "type" &&
          std::string_view(tag.attribute("v").value()) == "restriction") {
        ++graph.turn_restriction_count;
      }
    }
  }
  if (graph.relation_count > 0) {
    diag(Severity::Warning,
         fmt::format("{} relation(s) were not imported; this build reads nodes and ways only",
                     graph.relation_count));
  }
  if (graph.turn_restriction_count > 0) {
    // Called out separately: someone took the trouble to map these, and should
    // be told directly rather than left to discover it.
    diag(Severity::Warning,
         fmt::format("{} of those relations are turn restrictions, which are not carried into "
                     "the network's junction connectivity",
                     graph.turn_restriction_count));
  }

  // --- bounds --------------------------------------------------------------
  const pugi::xml_node bounds = root.child("bounds");
  double min_lon = 0.0;
  double min_lat = 0.0;
  double max_lon = 0.0;
  double max_lat = 0.0;
  if (bounds && read_coordinate(bounds, "minlon", min_lon) &&
      read_coordinate(bounds, "minlat", min_lat) && read_coordinate(bounds, "maxlon", max_lon) &&
      read_coordinate(bounds, "maxlat", max_lat)) {
    graph.bounds = {min_lon, min_lat, max_lon, max_lat};
  } else if (!graph.nodes.empty()) {
    // Derived from what is actually here rather than from what the file
    // claimed — a <bounds> from the query is a box that was ASKED for, and a
    // cropped extract routinely contains less.
    double lo_x = std::numeric_limits<double>::max();
    double lo_y = std::numeric_limits<double>::max();
    double hi_x = std::numeric_limits<double>::lowest();
    double hi_y = std::numeric_limits<double>::lowest();
    for (const auto& [id, point] : graph.nodes) {
      lo_x = std::min(lo_x, point.lon_deg);
      lo_y = std::min(lo_y, point.lat_deg);
      hi_x = std::max(hi_x, point.lon_deg);
      hi_y = std::max(hi_y, point.lat_deg);
    }
    graph.bounds = {lo_x, lo_y, hi_x, hi_y};
  }

  if (graph.ways.empty()) {
    diag(Severity::Warning, "extract contains no road ways");
  }
  return result;
}

} // namespace

std::string_view OsmWay::tag(std::string_view key) const {
  const auto found = std::ranges::find(
      tags, key, [](const auto& entry) -> std::string_view { return entry.first; });
  return found == tags.end() ? std::string_view{} : std::string_view(found->second);
}

bool OsmWay::has_tag(std::string_view key) const {
  return std::ranges::any_of(tags, [key](const auto& entry) { return entry.first == key; });
}

bool OsmWay::closed() const {
  return refs.size() > 2 && refs.front() == refs.back();
}

bool is_osm_extension(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  // .pbf is deliberately NOT here: a dialog that offers a file the reader
  // refuses is worse than one that does not list it.
  return ends_with_ci(name, ".osm") || ends_with_ci(name, ".osm.xml");
}

Expected<OsmParseResult>
parse_osm(std::string_view xml, std::string_view source_name, const OsmReadOptions& options) {
  pugi::xml_document document;
  const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
  if (!parsed) {
    return make_error(ErrorCode::MalformedXml,
                      fmt::format("{} at offset {}", parsed.description(), parsed.offset),
                      std::string(source_name));
  }
  return build(document, source_name, options);
}

Expected<OsmParseResult> load_osm(const std::filesystem::path& path,
                                  const OsmReadOptions& options) {
  const std::string name = path.filename().string();
  if (ends_with_ci(name, ".pbf")) {
    return make_error(ErrorCode::InvalidArgument, fmt::format(kPbfRefusal, name), name);
  }
  if (!is_osm_extension(path)) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("'{}' is not an OpenStreetMap XML file (expected .osm or .osm.xml)", name),
        name);
  }

  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return make_error(ErrorCode::FileNotFound, "file does not exist", name);
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return make_error(ErrorCode::IoFailure, "could not open file", name);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  return parse_osm(text, name, options);
}

} // namespace roadmaker::osm
