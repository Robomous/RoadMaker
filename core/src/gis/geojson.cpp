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

#include "roadmaker/gis/layer.hpp"

#include <fmt/format.h>

#include <json.hpp>
#include <string>
#include <vector>

#include "gis_common.hpp"

namespace roadmaker::gis {

namespace {

using nlohmann::json;

/// RFC 7946 §4: "the coordinate reference system for all GeoJSON coordinates is
/// a geographic CRS, using the WGS 84 datum, with longitude and latitude units
/// of decimal degrees." There is no CRS to negotiate — which is why GeoJSON is
/// the one supported format that cannot be ambiguous about where it is.
///
/// The pre-RFC `crs` member (GeoJSON 2008) could name something else. It was
/// removed from the standard, and honouring it would mean supporting a
/// deprecated extension; a file carrying a NON-WGS84 one is warned about and
/// still read as WGS84, because that is what every current producer means.
constexpr std::string_view kGeoJsonCrs = "EPSG:4326";

void append_ring(GisFeature& feature,
                 const json& coords,
                 std::vector<Diagnostic>& diagnostics,
                 std::string_view location) {
  feature.ring_starts.push_back(feature.vertices.size());
  for (const json& point : coords) {
    if (!point.is_array() || point.size() < 2 || !point[0].is_number() || !point[1].is_number()) {
      diagnostics.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = std::string(location),
          .message = "a coordinate was not a [longitude, latitude] pair and was skipped"});
      continue;
    }
    feature.vertices.push_back({point[0].get<double>(), point[1].get<double>()});
  }
}

/// Reads one GeoJSON geometry into zero or more features. Multi* geometries and
/// GeometryCollection expand into several, because a "feature" here is one
/// drawable run and keeping the multi-part grouping would buy nothing a
/// reference layer can use.
void read_geometry(const json& geometry,
                   const std::string& name,
                   std::vector<GisFeature>& out,
                   std::vector<Diagnostic>& diagnostics,
                   std::string_view location) {
  if (!geometry.is_object() || !geometry.contains("type") || !geometry["type"].is_string()) {
    diagnostics.push_back(Diagnostic{.severity = Severity::Warning,
                                     .location = std::string(location),
                                     .message = "a geometry had no type and was skipped"});
    return;
  }
  const std::string type = geometry["type"].get<std::string>();

  if (type == "GeometryCollection") {
    if (geometry.contains("geometries") && geometry["geometries"].is_array()) {
      for (const json& child : geometry["geometries"]) {
        read_geometry(child, name, out, diagnostics, location);
      }
    }
    return;
  }

  if (!geometry.contains("coordinates") || !geometry["coordinates"].is_array()) {
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = std::string(location),
        .message = fmt::format("a {} geometry had no coordinates and was skipped", type)});
    return;
  }
  const json& coords = geometry["coordinates"];

  const auto emit = [&](GisFeature::Geometry kind, const json& rings, bool nested) {
    GisFeature feature;
    feature.geometry = kind;
    feature.name = name;
    if (nested) {
      for (const json& ring : rings) {
        append_ring(feature, ring, diagnostics, location);
      }
    } else {
      append_ring(feature, rings, diagnostics, location);
    }
    if (!feature.vertices.empty()) {
      out.push_back(std::move(feature));
    }
  };

  if (type == "Point") {
    GisFeature feature;
    feature.geometry = GisFeature::Geometry::Point;
    feature.name = name;
    feature.ring_starts.push_back(0);
    if (coords.size() >= 2 && coords[0].is_number() && coords[1].is_number()) {
      feature.vertices.push_back({coords[0].get<double>(), coords[1].get<double>()});
      out.push_back(std::move(feature));
    }
    return;
  }
  if (type == "MultiPoint") {
    for (const json& point : coords) {
      read_geometry(
          json{{"type", "Point"}, {"coordinates", point}}, name, out, diagnostics, location);
    }
    return;
  }
  if (type == "LineString") {
    emit(GisFeature::Geometry::Line, coords, false);
    return;
  }
  if (type == "MultiLineString") {
    for (const json& line : coords) {
      emit(GisFeature::Geometry::Line, line, false);
    }
    return;
  }
  if (type == "Polygon") {
    emit(GisFeature::Geometry::Polygon, coords, true);
    return;
  }
  if (type == "MultiPolygon") {
    for (const json& polygon : coords) {
      emit(GisFeature::Geometry::Polygon, polygon, true);
    }
    return;
  }

  diagnostics.push_back(
      Diagnostic{.severity = Severity::Warning,
                 .location = std::string(location),
                 .message = fmt::format("geometry type \"{}\" is not a GeoJSON geometry and was "
                                        "skipped",
                                        type)});
}

/// `properties.name`, or the first string-valued property whose key looks like
/// a name. Purely cosmetic — a layer with no names is fully usable.
std::string feature_name(const json& properties) {
  if (!properties.is_object()) {
    return {};
  }
  for (const char* key : {"name", "Name", "NAME", "ref", "id"}) {
    if (properties.contains(key) && properties[key].is_string()) {
      return properties[key].get<std::string>();
    }
  }
  return {};
}

} // namespace

Expected<GisVectorParseResult> parse_geojson(std::string_view text, std::string_view source_name) {
  GisVectorParseResult result;
  result.layer.crs = std::string(kGeoJsonCrs);

  json root;
  try {
    root = json::parse(text);
  } catch (const json::parse_error& error) {
    // nlohmann throws; the kernel API does not (no exceptions across the public
    // boundary). This is that boundary.
    return make_error(ErrorCode::InvalidDocument,
                      fmt::format("not valid JSON: {}", error.what()),
                      std::string(source_name));
  }

  if (!root.is_object() || !root.contains("type") || !root["type"].is_string()) {
    return make_error(ErrorCode::InvalidDocument,
                      "not a GeoJSON document: the root object has no \"type\"",
                      std::string(source_name));
  }

  if (root.contains("crs")) {
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = std::string(source_name),
        .message = "the file carries a \"crs\" member, which GeoJSON removed in RFC 7946; its "
                   "coordinates are read as WGS 84 longitude/latitude as the standard requires"});
  }

  const std::string type = root["type"].get<std::string>();
  if (type == "FeatureCollection") {
    if (!root.contains("features") || !root["features"].is_array()) {
      return make_error(ErrorCode::InvalidDocument,
                        "a FeatureCollection with no \"features\" array",
                        std::string(source_name));
    }
    std::size_t index = 0;
    for (const json& feature : root["features"]) {
      const std::string location = fmt::format("features[{}]", index);
      ++index;
      if (!feature.is_object() || !feature.contains("geometry")) {
        result.diagnostics.push_back(
            Diagnostic{.severity = Severity::Warning,
                       .location = location,
                       .message = "a feature had no geometry and was skipped"});
        continue;
      }
      if (feature["geometry"].is_null()) {
        result.diagnostics.push_back(
            Diagnostic{.severity = Severity::Info,
                       .location = location,
                       .message = "a feature had a null geometry (legal in GeoJSON) and "
                                  "contributes nothing to draw"});
        continue;
      }
      const std::string name =
          feature.contains("properties") ? feature_name(feature["properties"]) : std::string{};
      read_geometry(feature["geometry"], name, result.layer.features, result.diagnostics, location);
    }
  } else if (type == "Feature") {
    if (!root.contains("geometry") || root["geometry"].is_null()) {
      return make_error(
          ErrorCode::InvalidDocument, "a Feature with no geometry", std::string(source_name));
    }
    const std::string name =
        root.contains("properties") ? feature_name(root["properties"]) : std::string{};
    read_geometry(root["geometry"], name, result.layer.features, result.diagnostics, "feature");
  } else {
    // A bare geometry object is legal GeoJSON too.
    read_geometry(root, std::string{}, result.layer.features, result.diagnostics, "geometry");
  }

  if (const Expected<void> capped = enforce_vertex_budget(result.layer, source_name);
      !capped.has_value()) {
    return make_error(capped.error().code, capped.error().message, capped.error().context);
  }

  recompute_bounds(result.layer);
  return result;
}

} // namespace roadmaker::gis
