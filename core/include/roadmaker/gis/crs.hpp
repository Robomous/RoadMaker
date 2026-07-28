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

#pragma once

// Coordinate reference systems, for reprojecting imported GIS data into the
// scene's world frame (p7-s2, #242).
//
// WHAT THIS SUPPORTS, AND WHY IT IS A CLOSED LIST.
// ADR-0010 rules that RoadMaker computes a bounded family of projections in
// closed form and refuses everything else BY NAME. This is `tmerc_origin()`'s
// rule widened: that function returns nullopt for a UTM string rather than be
// wrong by its 500 km false easting, and an importer that quietly
// mis-georeferenced an orthophoto would be the same defect with a picture
// attached. So:
//
//   - WGS84 geographic (lon/lat degrees)     — EPSG:4326, +proj=longlat
//   - Transverse Mercator                    — +proj=tmerc, incl. p7-s5's own
//   - UTM north/south zones                  — EPSG:326xx/327xx, +proj=utm
//   - Web Mercator                           — EPSG:3857, +proj=merc +a=6378137
//
// Everything else parses to `CrsKind::Opaque` with its text intact and is
// refused at `crs_transform`, naming what it read. PROJ is deliberately NOT a
// dependency; the escape hatch, and what it would cost, is #485.
//
// NO DATUM SHIFT IS PERFORMED OR CLAIMED. WGS84 and GRS80 are treated as one
// ellipsoid: they differ by roughly 0.1 mm in the semi-minor axis, four orders
// of magnitude below anything this editor authors. A source in a datum that
// genuinely needs a grid-shift transform is Opaque, not approximated.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/georeference.hpp"

#include <array>
#include <string>
#include <string_view>

namespace roadmaker::gis {

/// The bounded family. `Opaque` is not a failure — it is an honest reading of a
/// CRS this build carries but cannot compute with.
enum class CrsKind {
  Geographic,         ///< lon/lat degrees on the WGS84 ellipsoid
  TransverseMercator, ///< incl. UTM, which is a TM with fixed k_0/x_0/y_0
  WebMercator,        ///< spherical Mercator on a sphere of R = 6378137 m
  Opaque,             ///< carried verbatim; every transform involving it is refused
};

/// A parsed coordinate reference system.
///
/// The projection parameters carry PROJ's own names and defaults so a string
/// that omits `+k`/`+x_0`/`+y_0` describes the same projection as one that
/// spells them out. `text` is ALWAYS the verbatim input, including for Opaque —
/// that is what makes a refusal able to name what it read, and what lets an
/// importer round-trip a CRS it cannot interpret.
struct Crs {
  CrsKind kind = CrsKind::Opaque;
  double lat_0 = 0.0; ///< latitude of origin, degrees
  double lon_0 = 0.0; ///< central meridian, degrees
  double k_0 = 1.0;   ///< scale factor on the central meridian
  double x_0 = 0.0;   ///< false easting, metres
  double y_0 = 0.0;   ///< false northing, metres
  std::string text;

  [[nodiscard]] bool opaque() const { return kind == CrsKind::Opaque; }
};

/// Parses a CRS description. Accepts three spellings, because that is what the
/// three supported formats hand us:
///   - a PROJ string  (`+proj=utm +zone=31 +datum=WGS84 +units=m`) — `.prj` of
///     some tools, and `<geoReference>` itself
///   - an EPSG code   (`EPSG:32631`, `epsg:4326`) — GeoTIFF GeoKeys
///   - ESRI WKT       (`PROJCS["WGS_1984_UTM_Zone_31N", ...]`) — shapefile `.prj`
///
/// NEVER fails: an unreadable or unsupported description is `CrsKind::Opaque`
/// carrying the input text. Refusal is `crs_transform`'s job, so that the
/// message can name both ends of the transform that could not be built.
[[nodiscard]] RM_API Crs parse_crs(std::string_view text);

/// The CRS a scene's own coordinates are in, derived from its `<geoReference>`.
///
/// An empty georeference means a local Cartesian frame (§8.5: "if the
/// definition is missing, a local Cartesian coordinate system is assumed"), for
/// which no world-referenced transform exists — that is `Opaque` with empty
/// text, and importing georeferenced data into such a scene is refused with a
/// message that says to set a world origin first.
[[nodiscard]] RM_API Crs scene_crs(const GeoReference& georeference);

/// Maps source coordinates into the scene's frame.
///
/// `apply` takes the source's own axis order in metres or degrees as that CRS
/// defines them, and returns scene-frame x/y in metres.
///
/// `affine` reports whether the mapping is a pure translation/rotation/scale
/// over the region of interest. It is the difference between PLACING a raster
/// and RESAMPLING it, so it is part of the public surface rather than an
/// implementation detail: a caller that ignores it silently degrades imagery.
class RM_API CrsTransform {
public:
  CrsTransform() = default;
  CrsTransform(Crs from, Crs to);

  [[nodiscard]] std::array<double, 2> apply(double x, double y) const;

  /// Inverse of `apply`. Used to walk a target raster grid back into source
  /// pixels when resampling — the direction that avoids holes in the output.
  [[nodiscard]] std::array<double, 2> invert(double x, double y) const;

  [[nodiscard]] bool affine() const { return affine_; }

  [[nodiscard]] const Crs& source() const { return from_; }

  [[nodiscard]] const Crs& target() const { return to_; }

private:
  Crs from_{};
  Crs to_{};
  bool affine_ = false;
};

/// Builds the transform, or refuses naming the CRS it could not compute with.
/// The error message is user-facing and cites #485, which is the issue that
/// would lift the limitation — a refusal that only says "unsupported" tells a
/// user nothing about where the boundary is or whether it is moving.
[[nodiscard]] RM_API Expected<CrsTransform> crs_transform(const Crs& from, const Crs& to);

/// A short human-readable name: "UTM zone 31N", "WGS 84 geographic",
/// "Web Mercator", "Transverse Mercator (lat_0=52, lon_0=5)", or for an opaque
/// CRS the input text itself, elided if very long. Used in diagnostics and in
/// the editor's read-outs, so both say the same thing about the same file.
[[nodiscard]] RM_API std::string describe_crs(const Crs& crs);

/// Forward/inverse Transverse Mercator on the WGS84 ellipsoid (Krüger series,
/// sub-millimetre within a zone's normal range of validity). Exposed because
/// the tests pin them against published reference values independently of any
/// CRS plumbing; ordinary callers want `CrsTransform`.
///
/// `tmerc_forward` takes lon/lat in DEGREES and returns easting/northing in
/// metres including `k_0`/`x_0`/`y_0`; `tmerc_inverse` is its exact inverse.
[[nodiscard]] RM_API std::array<double, 2>
tmerc_forward(const Crs& crs, double longitude_deg, double latitude_deg);
[[nodiscard]] RM_API std::array<double, 2>
tmerc_inverse(const Crs& crs, double easting, double northing);

} // namespace roadmaker::gis
