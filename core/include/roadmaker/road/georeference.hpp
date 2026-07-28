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

/// The scene's world georeference — OpenDRIVE 1.9.0 §8.5 / 1.8.1 §8.5, which
/// are textually identical (p7-s5, #324).
///
/// WHAT THIS DOES NOT DO, and why. §8.5 describes a geodetic datum with a PROJ
/// string, and converting BETWEEN two such datums needs the PROJ library. PROJ
/// is not a dependency of this project and is deliberately deferred to p7-s2
/// (#242, GIS import), where the first consumer that genuinely has to transform
/// coordinates between two different reference systems appears. Bringing it in
/// here would add SQLite plus a runtime `proj.db` resource to a kernel that
/// builds from CMake alone and ships no runtime data, to serve no caller.
///
/// So this module models, carries and round-trips the georeference; it does not
/// interpret foreign projections. The one construction it DOES understand is
/// the one §8.5 recommends for exactly this situation:
///
/// > Alternatively define a custom projection using Transverse Mercator (TM).
///
/// `tmerc_projection(lat, lon)` builds a Transverse Mercator centred on the
/// scene's origin with no false easting or northing, which makes the local
/// OpenDRIVE coordinates and the projected coordinates THE SAME NUMBERS. A
/// scene georeferenced that way needs no transform to be correct, which is what
/// lets the whole sprint ship without PROJ. Any other CRS the user supplies is
/// stored verbatim and reported as opaque — an honest "we carry this, we do not
/// read it" — until #242 lands the library that can.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace roadmaker {

/// The `<header><offset>` element (§8.5, Table 17).
///
/// All four attributes are `required` in the standard, so this is ONE value
/// that is either present or absent — never a struct of per-field optionals.
/// An absent offset and an all-zero offset mean the same thing; the writer
/// prefers absence, because the standard makes the element optional and an
/// all-zero affine is noise in the file.
struct GeoOffset {
  double x = 0.0;   ///< inertial x offset [m]
  double y = 0.0;   ///< inertial y offset [m]
  double z = 0.0;   ///< inertial z offset [m]
  double hdg = 0.0; ///< heading offset, rotation about the resulting z axis [rad]

  /// True when applying this offset is the identity. §8.5 recommends supplying
  /// no heading at all, so this is the common case.
  [[nodiscard]] bool identity() const { return x == 0.0 && y == 0.0 && z == 0.0 && hdg == 0.0; }

  friend bool operator==(const GeoOffset&, const GeoOffset&) = default;
};

/// The scene's georeference: `<header><geoReference>` plus `<header><offset>`.
///
/// NOT an arena entity — there is exactly one per network, and it is document
/// metadata rather than a road-network object, so it lives on RoadNetwork
/// beside the height field and the preserved root userData.
///
/// A default-constructed value is the ABSENT georeference, and that case is
/// load-bearing: it means "this scene is a plain local Cartesian frame", which
/// is what every RoadMaker file has meant until now. An empty georeference
/// writes no `<geoReference>` and no `<offset>`, so every existing file stays
/// byte-identical. §8.5's own rule says the same thing from the reader's side:
/// "If the definition is missing, a local Cartesian coordinate system is
/// assumed."
struct GeoReference {
  /// The PROJ string, verbatim, exactly as it appeared between the
  /// `<geoReference>` tags (CDATA unwrapped, surrounding whitespace trimmed).
  /// Empty ⇒ no `<geoReference>` element is written.
  ///
  /// Stored as text on purpose. RoadMaker authors this string through
  /// `tmerc_projection`, and preserves anything else unread — see the header
  /// comment on why there is no parsed representation.
  std::string projection;

  std::optional<GeoOffset> offset;

  /// True when this scene has no georeference at all.
  [[nodiscard]] bool empty() const { return projection.empty() && !offset.has_value(); }

  friend bool operator==(const GeoReference&, const GeoReference&) = default;
};

/// §8.5's affine, local OpenDRIVE coordinates → world coordinates:
///
///     xWorld = xODR*cos(hdg) - yODR*sin(hdg) + xOffset
///     yWorld = xODR*sin(hdg) + yODR*cos(hdg) + yOffset
///     zWorld = zODR + zOffset
///
/// This is applied BEFORE any datum conversion (§8.5: "If an offset exists,
/// always apply the offset on the local ASAM OpenDRIVE coordinates to get the
/// world coordinates before converting the positions using PROJ").
[[nodiscard]] RM_API std::array<double, 3>
geo_to_world(const GeoOffset& offset, double x, double y, double z);

/// The exact inverse of `geo_to_world` — world coordinates back to local
/// OpenDRIVE coordinates.
[[nodiscard]] RM_API std::array<double, 3>
geo_to_local(const GeoOffset& offset, double x, double y, double z);

/// The Transverse Mercator projection string §8.5 recommends for a scene whose
/// local origin sits at (`latitude_deg`, `longitude_deg`).
///
/// `+k=1 +x_0=0 +y_0=0` is the whole point: the projection's origin IS the
/// scene's origin and there is no false easting or northing, so a local
/// OpenDRIVE coordinate and its projected coordinate are the same number and
/// no transform is ever needed. Scale distortion grows with distance from the
/// central meridian — about 1.2 ppm at 10 km, i.e. a centimetre — which is far
/// below the tolerances anything in this kernel works to.
///
/// The two angles are formatted with the shortest decimal that reads back as
/// the identical double, so `tmerc_origin` recovers them exactly.
///
/// Rejects a non-finite or out-of-range angle (latitude outside [-90, 90],
/// longitude outside [-180, 180]).
[[nodiscard]] RM_API Expected<std::string> tmerc_projection(double latitude_deg,
                                                            double longitude_deg);

/// The (latitude, longitude) a projection string places the scene origin at,
/// in degrees — or `std::nullopt` when the string is not one this build can
/// read.
///
/// It answers only for the family `tmerc_projection` emits: a `+proj=tmerc`
/// with unit scale and no false easting or northing. That is precisely the
/// family in which local and projected coordinates coincide, which is the only
/// claim this module can make without PROJ. Every other CRS — a UTM zone, an
/// EPSG-derived string, a WKT blob — returns nullopt, and callers must present
/// it as an opaque projection rather than guess at an origin.
///
/// Whitespace-insensitive and parameter-order-insensitive, so a string that
/// went through another tool's formatter still reads.
[[nodiscard]] RM_API std::optional<std::array<double, 2>>
tmerc_origin(std::string_view projection);

} // namespace roadmaker
