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

#include <cmath>
#include <vector>

#include "proj_string.hpp"

namespace roadmaker {

namespace {

using proj_detail::parse_double;
using proj_detail::proj_number_is;
using proj_detail::proj_parameters;
using proj_detail::proj_value;

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
