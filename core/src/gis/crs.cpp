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

#include "roadmaker/gis/crs.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../road/proj_string.hpp"

namespace roadmaker::gis {

namespace {

using proj_detail::parse_double;
using proj_detail::proj_number;
using proj_detail::proj_parameters;
using proj_detail::proj_value;
using proj_detail::ProjParam;

// --- Ellipsoid ------------------------------------------------------------
//
// WGS84. GRS80 is accepted as the same ellipsoid: the two differ by about
// 0.1 mm in the semi-minor axis (b = 6356752.314245 vs 6356752.314140), which
// is four orders of magnitude below anything this editor authors. This is an
// ELLIPSOID equivalence and not a DATUM one — a CRS on a different datum
// (NAD83, ED50, a national grid) is Opaque, because reconciling those needs a
// shift this build does not perform and does not claim to (ADR-0010).
constexpr double kSemiMajor = 6378137.0;
constexpr double kFlattening = 1.0 / 298.257223563;

/// Web Mercator's sphere: EPSG:3857 is defined on a sphere whose radius is
/// WGS84's semi-major axis, with geodetic latitude used as if it were
/// spherical. That "wrong on purpose" definition is the specification, not an
/// approximation we chose.
constexpr double kWebMercatorRadius = 6378137.0;

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

/// Krüger series coefficients, computed once from the flattening.
///
/// Sixth-order in the third flattening `n`, which holds sub-millimetre accuracy
/// out to a few degrees either side of the central meridian — comfortably
/// covering a UTM zone's range of validity, which is the region any of this is
/// used over. Far outside a zone the series degrades; that is a property of the
/// projection being misapplied, not of this code.
struct KrugerSeries {
  double A = 0.0;
  std::array<double, 4> alpha{};
  std::array<double, 4> beta{};
  std::array<double, 4> delta{};
};

const KrugerSeries& kruger() {
  static const KrugerSeries series = [] {
    KrugerSeries s;
    const double n = kFlattening / (2.0 - kFlattening);
    const double n2 = n * n;
    const double n3 = n2 * n;
    const double n4 = n3 * n;
    s.A = (kSemiMajor / (1.0 + n)) * (1.0 + (n2 / 4.0) + (n4 / 64.0));
    s.alpha = {(n / 2.0) - (2.0 / 3.0 * n2) + (5.0 / 16.0 * n3) + (41.0 / 180.0 * n4),
               (13.0 / 48.0 * n2) - (3.0 / 5.0 * n3) + (557.0 / 1440.0 * n4),
               (61.0 / 240.0 * n3) - (103.0 / 140.0 * n4),
               (49561.0 / 161280.0 * n4)};
    s.beta = {(n / 2.0) - (2.0 / 3.0 * n2) + (37.0 / 96.0 * n3) - (1.0 / 360.0 * n4),
              (1.0 / 48.0 * n2) + (1.0 / 15.0 * n3) - (437.0 / 1440.0 * n4),
              (17.0 / 480.0 * n3) - (37.0 / 840.0 * n4),
              (4397.0 / 161280.0 * n4)};
    s.delta = {(2.0 * n) - (2.0 / 3.0 * n2) - (2.0 * n3) + (116.0 / 45.0 * n4),
               (7.0 / 3.0 * n2) - (8.0 / 5.0 * n3) - (227.0 / 45.0 * n4),
               (56.0 / 15.0 * n3) - (136.0 / 35.0 * n4),
               (4279.0 / 630.0 * n4)};
    return s;
  }();
  return series;
}

/// The conformal-latitude term `ξ'` for a latitude on the central meridian.
/// Also the building block of the meridian arc, which is what makes a
/// `lat_0 != 0` Transverse Mercator work: its northing is measured from that
/// parallel, not from the equator.
double meridian_xi(double latitude_rad) {
  const KrugerSeries& s = kruger();
  const double n = kFlattening / (2.0 - kFlattening);
  const double two_sqrt_n = 2.0 * std::sqrt(n) / (1.0 + n);
  const double sin_lat = std::sin(latitude_rad);
  const double t = std::sinh(std::atanh(sin_lat) - (two_sqrt_n * std::atanh(two_sqrt_n * sin_lat)));
  double xi = std::atan(t);
  for (std::size_t j = 0; j < s.alpha.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    xi += s.alpha[j] * std::sin(k * std::atan(t));
  }
  return xi;
}

/// Northing offset of the projection's own origin parallel. Zero for UTM and
/// for every string p7-s5 emits (both use lat_0 = 0), so the common path pays
/// nothing for this generality.
double origin_xi(const Crs& crs) {
  if (crs.lat_0 == 0.0) {
    return 0.0;
  }
  return meridian_xi(crs.lat_0 * kDegToRad);
}

std::array<double, 2> web_mercator_forward(double longitude_deg, double latitude_deg) {
  const double clamped = std::clamp(latitude_deg, -89.999999, 89.999999);
  return {kWebMercatorRadius * longitude_deg * kDegToRad,
          kWebMercatorRadius *
              std::log(std::tan((std::numbers::pi / 4.0) + (clamped * kDegToRad / 2.0)))};
}

std::array<double, 2> web_mercator_inverse(double x, double y) {
  return {x / kWebMercatorRadius * kRadToDeg,
          ((2.0 * std::atan(std::exp(y / kWebMercatorRadius))) - (std::numbers::pi / 2.0)) *
              kRadToDeg};
}

// --- Parsing helpers ------------------------------------------------------

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string_view trim(std::string_view text) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == '\0';
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

/// A UTM zone's Transverse Mercator parameters. Zone 1 is centred on -177°, and
/// each zone is 6° wide, hence `6z - 183`.
Crs utm_crs(int zone, bool south, std::string text) {
  Crs crs;
  crs.kind = CrsKind::TransverseMercator;
  crs.lat_0 = 0.0;
  crs.lon_0 = (6.0 * static_cast<double>(zone)) - 183.0;
  crs.k_0 = 0.9996;
  crs.x_0 = 500000.0;
  crs.y_0 = south ? 10000000.0 : 0.0;
  crs.text = std::move(text);
  return crs;
}

/// True when the ellipsoid/datum named is one this build treats as WGS84.
/// Absence counts as yes: a bare `+proj=tmerc` with no datum is the shape
/// `tmerc_projection` itself emits minus its explicit `+datum=WGS84`, and
/// refusing it would refuse our own output read back through a foreign tool
/// that dropped the token.
bool datum_is_wgs84_family(const std::vector<ProjParam>& params) {
  const std::optional<std::string_view> datum = proj_value(params, "datum");
  if (datum.has_value()) {
    const std::string value = to_lower(*datum);
    return value == "wgs84" || value == "grs80";
  }
  const std::optional<std::string_view> ellps = proj_value(params, "ellps");
  if (ellps.has_value()) {
    const std::string value = to_lower(*ellps);
    return value == "wgs84" || value == "grs80";
  }
  // A sphere given explicitly as the WGS84 semi-major axis is Web Mercator's
  // shape, handled by its own branch; anything else spelled with +a/+b is a
  // custom ellipsoid we decline rather than round to WGS84.
  return !proj_value(params, "a").has_value() && !proj_value(params, "b").has_value() &&
         !proj_value(params, "R").has_value();
}

/// Linear units must be metres. A CRS in feet is not refused because the
/// conversion is hard — it is refused because silently reading US survey feet
/// as metres is the exact class of error ADR-0010 exists to prevent.
bool units_are_metres(const std::vector<ProjParam>& params) {
  const std::optional<std::string_view> units = proj_value(params, "units");
  if (units.has_value() && to_lower(*units) != "m") {
    return false;
  }
  const std::optional<std::string_view> to_meter = proj_value(params, "to_meter");
  if (to_meter.has_value()) {
    const std::optional<double> factor = parse_double(*to_meter);
    return factor.has_value() && *factor == 1.0;
  }
  return true;
}

Crs opaque(std::string_view text) {
  Crs crs;
  crs.kind = CrsKind::Opaque;
  crs.text = std::string(text);
  return crs;
}

Crs parse_epsg(int code, std::string_view text) {
  if (code == 4326) {
    Crs crs;
    crs.kind = CrsKind::Geographic;
    crs.text = std::string(text);
    return crs;
  }
  // 3857 is the standard code; 900913 and 102100 are the widely-emitted
  // pre-standard spellings of the identical projection, and refusing them would
  // refuse ordinary web-tile-derived imagery for a bookkeeping reason.
  if (code == 3857 || code == 900913 || code == 102100) {
    Crs crs;
    crs.kind = CrsKind::WebMercator;
    crs.text = std::string(text);
    return crs;
  }
  if (code >= 32601 && code <= 32660) {
    return utm_crs(code - 32600, false, std::string(text));
  }
  if (code >= 32701 && code <= 32760) {
    return utm_crs(code - 32700, true, std::string(text));
  }
  return opaque(text);
}

Crs parse_proj_string(std::string_view text) {
  const std::vector<ProjParam> params = proj_parameters(text);
  const std::optional<std::string_view> proj = proj_value(params, "proj");
  if (!proj.has_value()) {
    return opaque(text);
  }
  const std::string name = to_lower(*proj);

  if (name == "longlat" || name == "latlong") {
    if (!datum_is_wgs84_family(params)) {
      return opaque(text);
    }
    Crs crs;
    crs.kind = CrsKind::Geographic;
    crs.text = std::string(text);
    return crs;
  }

  if (name == "merc") {
    // Web Mercator is the spherical Mercator on the WGS84 semi-major axis.
    // An ellipsoidal Mercator is a different projection and stays opaque.
    const std::optional<std::string_view> radius = proj_value(params, "R");
    const std::optional<std::string_view> semi_a = proj_value(params, "a");
    const std::optional<std::string_view> semi_b = proj_value(params, "b");
    const auto is_wgs_radius = [](const std::optional<std::string_view>& raw) {
      if (!raw.has_value()) {
        return false;
      }
      const std::optional<double> value = parse_double(*raw);
      return value.has_value() && std::abs(*value - kWebMercatorRadius) < 1e-6;
    };
    const bool spherical =
        is_wgs_radius(radius) || (is_wgs_radius(semi_a) && is_wgs_radius(semi_b));
    if (!spherical || !units_are_metres(params)) {
      return opaque(text);
    }
    Crs crs;
    crs.kind = CrsKind::WebMercator;
    crs.text = std::string(text);
    return crs;
  }

  if (name == "utm") {
    const std::optional<std::string_view> zone_raw = proj_value(params, "zone");
    if (!zone_raw.has_value() || !datum_is_wgs84_family(params) || !units_are_metres(params)) {
      return opaque(text);
    }
    const std::optional<double> zone = parse_double(*zone_raw);
    if (!zone.has_value() || *zone < 1.0 || *zone > 60.0 || *zone != std::floor(*zone)) {
      return opaque(text);
    }
    const bool south = proj_value(params, "south").has_value();
    return utm_crs(static_cast<int>(*zone), south, std::string(text));
  }

  if (name == "tmerc") {
    if (!datum_is_wgs84_family(params) || !units_are_metres(params)) {
      return opaque(text);
    }
    const std::optional<double> lat_0 = proj_number(params, "lat_0", 0.0);
    const std::optional<double> lon_0 = proj_number(params, "lon_0", 0.0);
    // PROJ accepts `k` as a deprecated alias of `k_0`; whichever is present
    // wins, and `k` is consulted first because that is the spelling
    // `tmerc_projection` emits.
    std::optional<double> scale = proj_number(params, "k", 1.0);
    if (!proj_value(params, "k").has_value()) {
      scale = proj_number(params, "k_0", 1.0);
    }
    const std::optional<double> x_0 = proj_number(params, "x_0", 0.0);
    const std::optional<double> y_0 = proj_number(params, "y_0", 0.0);
    if (!lat_0.has_value() || !lon_0.has_value() || !scale.has_value() || !x_0.has_value() ||
        !y_0.has_value()) {
      return opaque(text);
    }
    if (*lat_0 < -90.0 || *lat_0 > 90.0 || *lon_0 < -180.0 || *lon_0 > 180.0 || *scale <= 0.0) {
      return opaque(text);
    }
    Crs crs;
    crs.kind = CrsKind::TransverseMercator;
    crs.lat_0 = *lat_0;
    crs.lon_0 = *lon_0;
    crs.k_0 = *scale;
    crs.x_0 = *x_0;
    crs.y_0 = *y_0;
    crs.text = std::string(text);
    return crs;
  }

  return opaque(text);
}

// --- Minimal ESRI WKT reading ---------------------------------------------
//
// Enough to recognise the bounded family in a shapefile's `.prj`, and no more.
// This is not a WKT parser: it does not build a tree, and anything it cannot
// confidently identify becomes Opaque rather than a guess.

/// Offset and length of the quoted string immediately following `keyword[`,
/// e.g. the name in `PROJECTION["Transverse_Mercator"]`.
///
/// Returns a SPAN rather than the text because the search runs over a lowercased
/// copy while user-facing output wants the original casing, and `to_lower`
/// preserves length so the same span indexes both.
std::optional<std::pair<std::size_t, std::size_t>> wkt_quoted_span(std::string_view lower,
                                                                   std::string_view keyword) {
  const std::size_t at = lower.find(keyword);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t open = lower.find('"', at + keyword.size());
  if (open == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t close = lower.find('"', open + 1);
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{open + 1, close - open - 1};
}

/// The lowercased quoted string, for the comparisons the parser makes.
std::optional<std::string> wkt_quoted_after(std::string_view lower, std::string_view keyword) {
  const std::optional<std::pair<std::size_t, std::size_t>> span = wkt_quoted_span(lower, keyword);
  if (!span.has_value()) {
    return std::nullopt;
  }
  return std::string(lower.substr(span->first, span->second));
}

/// The numeric value of `PARAMETER["<name>",<value>]`.
std::optional<double> wkt_parameter(std::string_view lower, std::string_view name) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = lower.find("parameter", pos);
    if (at == std::string_view::npos) {
      return std::nullopt;
    }
    const std::size_t open = lower.find('"', at);
    const std::size_t close = open == std::string_view::npos ? open : lower.find('"', open + 1);
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view found = lower.substr(open + 1, close - open - 1);
    if (found == name) {
      const std::size_t comma = lower.find(',', close);
      const std::size_t end = lower.find(']', close);
      if (comma == std::string_view::npos || end == std::string_view::npos || comma > end) {
        return std::nullopt;
      }
      return parse_double(trim(lower.substr(comma + 1, end - comma - 1)));
    }
    pos = close + 1;
  }
}

bool wkt_datum_is_wgs84_family(std::string_view lower) {
  const std::optional<std::string> datum = wkt_quoted_after(lower, "datum[");
  if (!datum.has_value()) {
    return false;
  }
  // ESRI spells its datum names with a leading D_; OGC does not.
  return *datum == "wgs_1984" || *datum == "d_wgs_1984" || *datum == "wgs 1984" ||
         *datum == "d_wgs 1984";
}

Crs parse_wkt(std::string_view text) {
  const std::string lower = to_lower(text);

  if (!wkt_datum_is_wgs84_family(lower)) {
    return opaque(text);
  }

  // A GEOGCS with no PROJCS wrapper is geographic lon/lat.
  if (lower.find("projcs") == std::string::npos) {
    if (lower.find("geogcs") == std::string::npos) {
      return opaque(text);
    }
    Crs crs;
    crs.kind = CrsKind::Geographic;
    crs.text = std::string(text);
    return crs;
  }

  // Linear units must be metres, for the same reason as in a PROJ string. A
  // PROJCS carries two UNIT[] nodes — the inner GEOGCS's angular one and the
  // projected linear one — and it is the LAST that governs the coordinates.
  const std::size_t last_unit = lower.rfind("unit[");
  if (last_unit != std::string::npos) {
    const std::optional<std::string> linear =
        wkt_quoted_after(std::string_view(lower).substr(last_unit), "unit[");
    if (linear.has_value() && *linear != "meter" && *linear != "metre" && *linear != "m") {
      return opaque(text);
    }
  }

  const std::optional<std::string> projection = wkt_quoted_after(lower, "projection[");
  if (!projection.has_value()) {
    return opaque(text);
  }

  if (*projection == "mercator_auxiliary_sphere") {
    Crs crs;
    crs.kind = CrsKind::WebMercator;
    crs.text = std::string(text);
    return crs;
  }

  if (*projection == "transverse_mercator") {
    Crs crs;
    crs.kind = CrsKind::TransverseMercator;
    crs.lat_0 = wkt_parameter(lower, "latitude_of_origin").value_or(0.0);
    crs.lon_0 = wkt_parameter(lower, "central_meridian").value_or(0.0);
    crs.k_0 = wkt_parameter(lower, "scale_factor").value_or(1.0);
    crs.x_0 = wkt_parameter(lower, "false_easting").value_or(0.0);
    crs.y_0 = wkt_parameter(lower, "false_northing").value_or(0.0);
    crs.text = std::string(text);
    if (crs.lat_0 < -90.0 || crs.lat_0 > 90.0 || crs.lon_0 < -180.0 || crs.lon_0 > 180.0 ||
        crs.k_0 <= 0.0) {
      return opaque(text);
    }
    return crs;
  }

  return opaque(text);
}

/// Recovers a UTM zone from a Transverse Mercator whose parameters are exactly
/// UTM's. Used only for naming — the arithmetic never depends on it.
std::optional<std::pair<int, bool>> utm_zone_of(const Crs& crs) {
  if (crs.kind != CrsKind::TransverseMercator || crs.lat_0 != 0.0 || crs.k_0 != 0.9996 ||
      crs.x_0 != 500000.0) {
    return std::nullopt;
  }
  const bool south = crs.y_0 == 10000000.0;
  if (!south && crs.y_0 != 0.0) {
    return std::nullopt;
  }
  const double zone = (crs.lon_0 + 183.0) / 6.0;
  if (zone < 1.0 || zone > 60.0 || zone != std::floor(zone)) {
    return std::nullopt;
  }
  return std::pair<int, bool>{static_cast<int>(zone), south};
}

} // namespace

std::array<double, 2> tmerc_forward(const Crs& crs, double longitude_deg, double latitude_deg) {
  const KrugerSeries& s = kruger();
  const double n = kFlattening / (2.0 - kFlattening);
  const double two_sqrt_n = 2.0 * std::sqrt(n) / (1.0 + n);

  const double lat = latitude_deg * kDegToRad;
  const double dlon = (longitude_deg - crs.lon_0) * kDegToRad;
  const double sin_lat = std::sin(lat);
  const double t = std::sinh(std::atanh(sin_lat) - (two_sqrt_n * std::atanh(two_sqrt_n * sin_lat)));

  const double xi_prime = std::atan2(t, std::cos(dlon));
  const double eta_prime = std::atanh(std::sin(dlon) / std::hypot(1.0, t));

  double xi = xi_prime;
  double eta = eta_prime;
  for (std::size_t j = 0; j < s.alpha.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    xi += s.alpha[j] * std::sin(k * xi_prime) * std::cosh(k * eta_prime);
    eta += s.alpha[j] * std::cos(k * xi_prime) * std::sinh(k * eta_prime);
  }

  return {crs.x_0 + (crs.k_0 * s.A * eta), crs.y_0 + (crs.k_0 * s.A * (xi - origin_xi(crs)))};
}

std::array<double, 2> tmerc_inverse(const Crs& crs, double easting, double northing) {
  const KrugerSeries& s = kruger();

  const double xi = ((northing - crs.y_0) / (crs.k_0 * s.A)) + origin_xi(crs);
  const double eta = (easting - crs.x_0) / (crs.k_0 * s.A);

  double xi_prime = xi;
  double eta_prime = eta;
  for (std::size_t j = 0; j < s.beta.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    xi_prime -= s.beta[j] * std::sin(k * xi) * std::cosh(k * eta);
    eta_prime -= s.beta[j] * std::cos(k * xi) * std::sinh(k * eta);
  }

  const double chi = std::asin(std::clamp(std::sin(xi_prime) / std::cosh(eta_prime), -1.0, 1.0));
  double lat = chi;
  for (std::size_t j = 0; j < s.delta.size(); ++j) {
    const double k = 2.0 * static_cast<double>(j + 1);
    lat += s.delta[j] * std::sin(k * chi);
  }

  const double lon = crs.lon_0 + (std::atan2(std::sinh(eta_prime), std::cos(xi_prime)) * kRadToDeg);
  return {lon, lat * kRadToDeg};
}

Crs parse_crs(std::string_view text) {
  const std::string_view trimmed = trim(text);
  if (trimmed.empty()) {
    return opaque(trimmed);
  }

  const std::string lower = to_lower(trimmed);

  if (lower.starts_with("epsg:") || lower.starts_with("urn:ogc:def:crs:epsg:")) {
    const std::size_t colon = lower.rfind(':');
    const std::optional<double> code = parse_double(trim(trimmed.substr(colon + 1)));
    if (!code.has_value() || *code != std::floor(*code)) {
      return opaque(trimmed);
    }
    return parse_epsg(static_cast<int>(*code), trimmed);
  }

  if (trimmed.front() == '+') {
    return parse_proj_string(trimmed);
  }

  if (lower.find("projcs") != std::string::npos || lower.find("geogcs") != std::string::npos) {
    return parse_wkt(trimmed);
  }

  return opaque(trimmed);
}

Crs scene_crs(const GeoReference& georeference) {
  // §8.5: a missing projection means a local Cartesian frame. That is a real
  // and common state — every scene starts there — so it is Opaque with EMPTY
  // text, which `crs_transform` turns into a message telling the user to set a
  // world origin rather than one complaining about their file.
  if (georeference.projection.empty()) {
    return opaque(std::string_view{});
  }
  return parse_crs(georeference.projection);
}

CrsTransform::CrsTransform(Crs from, Crs to) : from_(std::move(from)), to_(std::move(to)) {
  // A mapping is affine exactly when both ends are the same projection family
  // AND agree on the parameters that curve it. Two Transverse Mercators on the
  // same central meridian differ only by scale and false origin, which is an
  // affine map; on DIFFERENT central meridians they do not, and a raster placed
  // as if they did would shear across its own extent.
  if (from_.kind != to_.kind) {
    affine_ = false;
  } else {
    switch (from_.kind) {
    case CrsKind::Geographic:
    case CrsKind::WebMercator:
      affine_ = true;
      break;
    case CrsKind::TransverseMercator:
      affine_ = from_.lat_0 == to_.lat_0 && from_.lon_0 == to_.lon_0;
      break;
    case CrsKind::Opaque:
      affine_ = false;
      break;
    }
  }
}

std::array<double, 2> CrsTransform::apply(double x, double y) const {
  // Everything routes through geographic lon/lat. One hub means one place each
  // projection's forward and inverse can disagree with itself, and the
  // round-trip test covers all of them at once.
  double lon = 0.0;
  double lat = 0.0;
  switch (from_.kind) {
  case CrsKind::Geographic:
    lon = x;
    lat = y;
    break;
  case CrsKind::TransverseMercator: {
    const std::array<double, 2> geo = tmerc_inverse(from_, x, y);
    lon = geo[0];
    lat = geo[1];
    break;
  }
  case CrsKind::WebMercator: {
    const std::array<double, 2> geo = web_mercator_inverse(x, y);
    lon = geo[0];
    lat = geo[1];
    break;
  }
  case CrsKind::Opaque:
    return {x, y};
  }

  switch (to_.kind) {
  case CrsKind::Geographic:
    return {lon, lat};
  case CrsKind::TransverseMercator:
    return tmerc_forward(to_, lon, lat);
  case CrsKind::WebMercator:
    return web_mercator_forward(lon, lat);
  case CrsKind::Opaque:
    break;
  }
  return {x, y};
}

std::array<double, 2> CrsTransform::invert(double x, double y) const {
  return CrsTransform(to_, from_).apply(x, y);
}

Expected<CrsTransform> crs_transform(const Crs& from, const Crs& to) {
  if (to.opaque()) {
    if (to.text.empty()) {
      return make_error(ErrorCode::InvalidArgument,
                        "the scene has no georeference, so imported data has nowhere to land — set "
                        "a world origin in Edit ▸ World Georeference first",
                        "crs");
    }
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("the scene's coordinate reference system ({}) is outside the family this "
                    "build can compute with; see issue #485",
                    describe_crs(to)),
        "crs");
  }
  if (from.opaque()) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("coordinate reference system {} is outside the family this build can compute "
                    "with — supported: WGS 84 geographic, UTM, Transverse Mercator, Web Mercator; "
                    "see issue #485",
                    describe_crs(from)),
        "crs");
  }
  return CrsTransform(from, to);
}

std::string describe_crs(const Crs& crs) {
  switch (crs.kind) {
  case CrsKind::Geographic:
    return "WGS 84 geographic (lon/lat)";
  case CrsKind::WebMercator:
    return "Web Mercator";
  case CrsKind::TransverseMercator: {
    const std::optional<std::pair<int, bool>> zone = utm_zone_of(crs);
    if (zone.has_value()) {
      return fmt::format("UTM zone {}{}", zone->first, zone->second ? 'S' : 'N');
    }
    return fmt::format("Transverse Mercator (lat_0={}, lon_0={})", crs.lat_0, crs.lon_0);
  }
  case CrsKind::Opaque:
    break;
  }
  if (crs.text.empty()) {
    return "none (local Cartesian)";
  }

  // A WKT blob must not be truncated from the front. Its first 80 characters
  // are the GEOGCS/DATUM boilerplate that every WKT shares, so an elision
  // shows the user the one part that identifies nothing while hiding the part
  // that identifies everything. Report its NAME and its projection instead —
  // which is what the user recognises and what a support question needs.
  const std::string lower = to_lower(crs.text);
  const bool is_wkt =
      lower.find("projcs[") != std::string::npos || lower.find("geogcs[") != std::string::npos;
  if (is_wkt) {
    // Spans into the ORIGINAL text, so the user sees "NAD27_Texas_North" as
    // they wrote it rather than the parser's lowercased working copy.
    const auto quoted = [&](std::string_view keyword) -> std::optional<std::string> {
      const std::optional<std::pair<std::size_t, std::size_t>> span =
          wkt_quoted_span(lower, keyword);
      if (!span.has_value()) {
        return std::nullopt;
      }
      return crs.text.substr(span->first, span->second);
    };
    const std::optional<std::string> name =
        quoted(lower.find("projcs[") != std::string::npos ? "projcs[" : "geogcs[");
    const std::optional<std::string> projection = quoted("projection[");
    if (name.has_value() && projection.has_value()) {
      return fmt::format("\"{}\" ({})", *name, *projection);
    }
    if (name.has_value()) {
      return fmt::format("\"{}\"", *name);
    }
  }

  constexpr std::size_t kMaxQuoted = 80;
  if (crs.text.size() > kMaxQuoted) {
    return fmt::format("\"{}…\"", crs.text.substr(0, kMaxQuoted));
  }
  return fmt::format("\"{}\"", crs.text);
}

} // namespace roadmaker::gis
