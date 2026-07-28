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

#include "roadmaker/road/georeference.hpp"

#include <fmt/format.h>

#include <fast_float/fast_float.h>

#include <cmath>
#include <system_error>
#include <vector>

namespace roadmaker {

namespace {

/// Locale-independent double parsing, rejecting trailing garbage — the same
/// contract as the reader's `to_double`. std::stod is locale-dependent and must
/// never touch a projection string, which is machine data in every locale.
std::optional<double> parse_double(std::string_view text) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  double value{};
  const auto result = fast_float::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

/// Splits a PROJ string into its `+key=value` and bare `+key` parameters.
/// Order-insensitive and whitespace-insensitive by construction, so a string
/// that has been through another tool's formatter still reads.
std::vector<std::pair<std::string_view, std::string_view>>
proj_parameters(std::string_view projection) {
  std::vector<std::pair<std::string_view, std::string_view>> params;
  std::size_t pos = 0;
  while (pos < projection.size()) {
    const std::size_t start = projection.find('+', pos);
    if (start == std::string_view::npos) {
      break;
    }
    std::size_t end = start + 1;
    while (end < projection.size() && projection[end] != ' ' && projection[end] != '\t' &&
           projection[end] != '\r' && projection[end] != '\n') {
      ++end;
    }
    const std::string_view token = projection.substr(start + 1, end - start - 1);
    const std::size_t eq = token.find('=');
    if (eq == std::string_view::npos) {
      params.emplace_back(token, std::string_view{});
    } else {
      params.emplace_back(token.substr(0, eq), token.substr(eq + 1));
    }
    pos = end;
  }
  return params;
}

/// The value of `key`, or nullopt when it is absent. A key repeated in the
/// string yields its FIRST occurrence, matching how PROJ itself resolves
/// duplicates.
std::optional<std::string_view>
proj_value(const std::vector<std::pair<std::string_view, std::string_view>>& params,
           std::string_view key) {
  for (const auto& [name, value] : params) {
    if (name == key) {
      return value;
    }
  }
  return std::nullopt;
}

/// True when `key` is absent, or present with a value that parses to `expected`.
/// Absence counts as a match because PROJ defaults `k`, `x_0` and `y_0` to
/// exactly the values this predicate is asked about, so a string that omits
/// them describes the same projection as one that spells them out.
bool proj_number_is(const std::vector<std::pair<std::string_view, std::string_view>>& params,
                    std::string_view key,
                    double expected) {
  const std::optional<std::string_view> raw = proj_value(params, key);
  if (!raw.has_value()) {
    return true;
  }
  const std::optional<double> value = parse_double(*raw);
  return value.has_value() && *value == expected;
}

/// Shortest decimal that reads back as the identical double.
///
/// fmt's default `{}` for a floating-point value is the shortest round-tripping
/// representation, which is the property `tmerc_origin` depends on: an angle
/// written here must parse back bit-for-bit or the origin the UI shows drifts
/// from the origin the file records. A fixed precision cannot do this — the
/// same trap fmt-s1 (#325) hit when nine significant digits turned 0.8 into
/// 0.800000012.
std::string shortest(double value) {
  return fmt::format("{}", value);
}

} // namespace

std::array<double, 3> geo_to_world(const GeoOffset& offset, double x, double y, double z) {
  const double cos_h = std::cos(offset.hdg);
  const double sin_h = std::sin(offset.hdg);
  return {(x * cos_h) - (y * sin_h) + offset.x, (x * sin_h) + (y * cos_h) + offset.y, z + offset.z};
}

std::array<double, 3> geo_to_local(const GeoOffset& offset, double x, double y, double z) {
  // The inverse rotation is the rotation by -hdg, applied after undoing the
  // translation. Written with cos/sin of the SAME angle rather than of -hdg so
  // the two directions cannot drift through different library calls.
  const double cos_h = std::cos(offset.hdg);
  const double sin_h = std::sin(offset.hdg);
  const double dx = x - offset.x;
  const double dy = y - offset.y;
  return {(dx * cos_h) + (dy * sin_h), (-dx * sin_h) + (dy * cos_h), z - offset.z};
}

Expected<std::string> tmerc_projection(double latitude_deg, double longitude_deg) {
  if (!std::isfinite(latitude_deg) || !std::isfinite(longitude_deg)) {
    return make_error(ErrorCode::InvalidArgument,
                      "world origin latitude and longitude must be finite",
                      "georeference");
  }
  if (latitude_deg < -90.0 || latitude_deg > 90.0) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("world origin latitude {} is outside [-90, 90]", latitude_deg),
                      "georeference");
  }
  if (longitude_deg < -180.0 || longitude_deg > 180.0) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("world origin longitude {} is outside [-180, 180]", longitude_deg),
        "georeference");
  }
  // Parameter order is fixed so the same origin always produces byte-identical
  // output — the writer's determinism guarantee reaches into this string.
  //
  // NO `+no_defs`, deliberately, even though §8.5's own example carries it.
  // esmini v3.5.0 REFUSES it outright — "Unsupported geo reference attr:
  // +no_defs" — and a string RoadMaker generates by default that a major
  // consumer rejects is a bad default, whatever the example does. Nothing is
  // lost: `+no_defs` is a PROJ.4-era flag telling PROJ not to consult the
  // `proj_def.dat` defaults file, and that file was removed in PROJ 6, so
  // every PROJ this project could target accepts it and ignores it. Found by
  // the esmini smoke gate, which is precisely what it is for.
  return fmt::format("+proj=tmerc +lat_0={} +lon_0={} +k=1 +x_0=0 +y_0=0 +datum=WGS84 +units=m",
                     shortest(latitude_deg),
                     shortest(longitude_deg));
}

std::optional<std::array<double, 2>> tmerc_origin(std::string_view projection) {
  const std::vector<std::pair<std::string_view, std::string_view>> params =
      proj_parameters(projection);

  const std::optional<std::string_view> proj = proj_value(params, "proj");
  if (!proj.has_value() || *proj != "tmerc") {
    return std::nullopt;
  }
  // Unit scale and no false easting/northing is what makes local coordinates
  // and projected coordinates the same numbers. A tmerc that shifts or scales
  // its grid is a projection we carry but cannot resolve an origin for.
  if (!proj_number_is(params, "k", 1.0) || !proj_number_is(params, "k_0", 1.0) ||
      !proj_number_is(params, "x_0", 0.0) || !proj_number_is(params, "y_0", 0.0)) {
    return std::nullopt;
  }

  const std::optional<std::string_view> lat_raw = proj_value(params, "lat_0");
  const std::optional<std::string_view> lon_raw = proj_value(params, "lon_0");
  if (!lat_raw.has_value() || !lon_raw.has_value()) {
    return std::nullopt;
  }
  const std::optional<double> lat = parse_double(*lat_raw);
  const std::optional<double> lon = parse_double(*lon_raw);
  if (!lat.has_value() || !lon.has_value()) {
    return std::nullopt;
  }
  if (*lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) {
    return std::nullopt;
  }
  return std::array<double, 2>{*lat, *lon};
}

} // namespace roadmaker
