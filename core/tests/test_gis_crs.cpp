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
#include "roadmaker/road/georeference.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace roadmaker;
using namespace roadmaker::gis;

namespace {

Crs utm31n() {
  return parse_crs("EPSG:32631");
}

} // namespace

// --- The bounded family, recognised in all three spellings -----------------

TEST(GisCrs, RecognisesEpsgCodes) {
  EXPECT_EQ(parse_crs("EPSG:4326").kind, CrsKind::Geographic);
  EXPECT_EQ(parse_crs("epsg:3857").kind, CrsKind::WebMercator);
  EXPECT_EQ(parse_crs("EPSG:32631").kind, CrsKind::TransverseMercator);
  EXPECT_EQ(parse_crs("EPSG:32731").kind, CrsKind::TransverseMercator);
}

TEST(GisCrs, RecognisesProjStrings) {
  EXPECT_EQ(parse_crs("+proj=longlat +datum=WGS84").kind, CrsKind::Geographic);
  EXPECT_EQ(parse_crs("+proj=utm +zone=31 +datum=WGS84 +units=m").kind,
            CrsKind::TransverseMercator);
  EXPECT_EQ(parse_crs("+proj=tmerc +lat_0=52 +lon_0=5 +k=1 +x_0=0 +y_0=0 +datum=WGS84").kind,
            CrsKind::TransverseMercator);
}

TEST(GisCrs, RecognisesEsriWkt) {
  const Crs crs =
      parse_crs("PROJCS[\"WGS_1984_UTM_Zone_31N\",GEOGCS[\"GCS_WGS_1984\","
                "DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]]],"
                "PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"False_Easting\",500000.0],"
                "PARAMETER[\"Central_Meridian\",3.0],PARAMETER[\"Scale_Factor\",0.9996],"
                "PARAMETER[\"Latitude_Of_Origin\",0.0],UNIT[\"Meter\",1.0]]");
  ASSERT_EQ(crs.kind, CrsKind::TransverseMercator);
  EXPECT_DOUBLE_EQ(crs.lon_0, 3.0);
  EXPECT_DOUBLE_EQ(crs.k_0, 0.9996);
  EXPECT_DOUBLE_EQ(crs.x_0, 500000.0);
}

TEST(GisCrs, UtmZoneParametersAreCorrect) {
  // Zone 31's central meridian is 3°E. An off-by-one in the 6z-183 formula
  // moves every import by a whole zone, which is 400 km at this latitude.
  const Crs crs = utm31n();
  EXPECT_DOUBLE_EQ(crs.lon_0, 3.0);
  EXPECT_DOUBLE_EQ(crs.lat_0, 0.0);
  EXPECT_DOUBLE_EQ(crs.k_0, 0.9996);
  EXPECT_DOUBLE_EQ(crs.x_0, 500000.0);
  EXPECT_DOUBLE_EQ(crs.y_0, 0.0);

  EXPECT_DOUBLE_EQ(parse_crs("EPSG:32601").lon_0, -177.0);
  EXPECT_DOUBLE_EQ(parse_crs("EPSG:32660").lon_0, 177.0);
  // South zones differ only by the 10 000 km false northing.
  EXPECT_DOUBLE_EQ(parse_crs("EPSG:32731").y_0, 10000000.0);
}

// --- What is refused, and that it is refused BY NAME -----------------------

TEST(GisCrs, RefusesOutOfFamilyCrs) {
  for (const char* text : {
           "EPSG:27700",                                 // British National Grid (OSGB36 datum)
           "EPSG:4269",                                  // NAD83 geographic — a DIFFERENT datum
           "+proj=lcc +lat_1=33 +lat_2=45 +datum=NAD27", // Lambert on NAD27
           "+proj=utm +zone=31 +datum=NAD27",            // right projection, wrong datum
           "+proj=merc +a=6371000 +b=6371000",           // a sphere, but not Web Mercator's
           "not a coordinate system at all",
       }) {
    EXPECT_EQ(parse_crs(text).kind, CrsKind::Opaque) << text;
    // The verbatim text always survives, which is what lets a refusal name it.
    EXPECT_EQ(parse_crs(text).text, text) << text;
  }
}

TEST(GisCrs, RefusesNonMetreLinearUnits) {
  // Reading US survey feet as metres is the exact silent-error class ADR-0010
  // exists to prevent, so a unit we do not convert must be Opaque.
  EXPECT_EQ(parse_crs("+proj=utm +zone=31 +datum=WGS84 +units=us-ft").kind, CrsKind::Opaque);
  EXPECT_EQ(parse_crs("+proj=tmerc +lat_0=0 +lon_0=3 +datum=WGS84 +to_meter=0.3048").kind,
            CrsKind::Opaque);
}

TEST(GisCrs, RefusalNamesTheCoordinateSystemAndCitesTheFollowUp) {
  const Crs from = parse_crs("+proj=lcc +lat_1=33 +datum=NAD27");
  const Expected<CrsTransform> transform = crs_transform(from, utm31n());
  ASSERT_FALSE(transform.has_value());
  EXPECT_NE(transform.error().message.find("+proj=lcc"), std::string::npos)
      << transform.error().message;
  EXPECT_NE(transform.error().message.find("#485"), std::string::npos) << transform.error().message;
}

TEST(GisCrs, SceneWithNoGeoreferenceIsRefusedWithActionableWording) {
  // A local Cartesian scene is the state EVERY scene starts in, so this
  // message must tell the user what to do rather than complain about the file.
  const Expected<CrsTransform> transform = crs_transform(utm31n(), scene_crs(GeoReference{}));
  ASSERT_FALSE(transform.has_value());
  EXPECT_NE(transform.error().message.find("World Georeference"), std::string::npos)
      << transform.error().message;
}

// --- The projection arithmetic ---------------------------------------------

TEST(GisCrs, TransverseMercatorMatchesAnIndependentSeriesExpansion) {
  // Amsterdam (4.8952°E, 52.3702°N) in UTM zone 31N.
  //
  // These values come from Snyder's series (USGS Professional Paper 1395,
  // §8) — a DIFFERENT expansion from the Krüger series the kernel uses, in a
  // different variable (e'² rather than the third flattening n). Two
  // independent formulations landing within a millimetre of each other is the
  // evidence; a round-trip test is not, because a forward and inverse that are
  // wrong in the same way round-trip perfectly. This test earned its keep
  // immediately: it failed on first run against hand-written "reference"
  // numbers, and recomputing them from Snyder showed the numbers were wrong
  // and the kernel was right.
  const std::array<double, 2> en = tmerc_forward(utm31n(), 4.8952, 52.3702);
  EXPECT_NEAR(en[0], 629024.573, 0.002);
  EXPECT_NEAR(en[1], 5803904.473, 0.002);
}

TEST(GisCrs, TransverseMercatorRoundTripsToSubMillimetre) {
  const Crs crs = utm31n();
  for (const auto& [lon, lat] : {std::pair{3.0, 0.0},
                                 std::pair{4.8952, 52.3702},
                                 std::pair{1.5, -33.9},
                                 std::pair{5.9, 71.0}}) {
    const std::array<double, 2> en = tmerc_forward(crs, lon, lat);
    const std::array<double, 2> back = tmerc_inverse(crs, en[0], en[1]);
    EXPECT_NEAR(back[0], lon, 1e-9) << lon << ", " << lat;
    EXPECT_NEAR(back[1], lat, 1e-9) << lon << ", " << lat;
  }
}

TEST(GisCrs, TransverseMercatorHonoursANonZeroLatitudeOfOrigin) {
  // A tmerc whose origin parallel is not the equator measures northing from
  // that parallel. Dropping the meridian-arc correction leaves an error of
  // millions of metres, so this is the one place the series' generality bites.
  const Crs at_origin = parse_crs("+proj=tmerc +lat_0=52 +lon_0=5 +k=1 +datum=WGS84");
  const std::array<double, 2> here = tmerc_forward(at_origin, 5.0, 52.0);
  EXPECT_NEAR(here[0], 0.0, 1e-6);
  EXPECT_NEAR(here[1], 0.0, 1e-6);
}

TEST(GisCrs, SceneOriginProjectionPutsTheOriginAtZero) {
  // p7-s5's whole construction: local coordinates ARE projected coordinates.
  const Expected<std::string> projection = tmerc_projection(52.3702, 4.8952);
  ASSERT_TRUE(projection.has_value());
  const Crs crs = parse_crs(*projection);
  ASSERT_EQ(crs.kind, CrsKind::TransverseMercator);
  const std::array<double, 2> at = tmerc_forward(crs, 4.8952, 52.3702);
  EXPECT_NEAR(at[0], 0.0, 1e-6);
  EXPECT_NEAR(at[1], 0.0, 1e-6);
}

TEST(GisCrs, WebMercatorMatchesItsDefinition) {
  const Crs crs = parse_crs("EPSG:3857");
  const CrsTransform to_geographic(crs, parse_crs("EPSG:4326"));
  // The equator/prime-meridian origin, and the classic x = R·λ at 180°.
  const std::array<double, 2> origin = CrsTransform(parse_crs("EPSG:4326"), crs).apply(0.0, 0.0);
  EXPECT_NEAR(origin[0], 0.0, 1e-9);
  EXPECT_NEAR(origin[1], 0.0, 1e-9);
  const std::array<double, 2> back = to_geographic.apply(20037508.342789244, 0.0);
  EXPECT_NEAR(back[0], 180.0, 1e-6);
}

// --- Affinity: the placed-vs-resampled decision ----------------------------

TEST(GisCrs, SameProjectionIsAffineAndDifferentCentralMeridiansAreNot) {
  const Crs zone31 = utm31n();
  const Crs zone32 = parse_crs("EPSG:32632");
  const Crs shifted = parse_crs("+proj=tmerc +lat_0=0 +lon_0=3 +k=1 +x_0=0 +y_0=0 +datum=WGS84");

  EXPECT_TRUE(CrsTransform(zone31, zone31).affine());
  // Same central meridian, different scale and false origin: still affine.
  EXPECT_TRUE(CrsTransform(zone31, shifted).affine());
  // Different central meridian: genuinely curved, so a raster must resample.
  EXPECT_FALSE(CrsTransform(zone31, zone32).affine());
  EXPECT_FALSE(CrsTransform(parse_crs("EPSG:4326"), zone31).affine());
}

TEST(GisCrs, AnAffineTransformIsActuallyAffine) {
  // The claim `affine()` makes is checkable: sample the map on a grid and
  // confirm it really is a linear map plus a translation. If this ever fails,
  // rasters are being placed that should be resampled.
  const CrsTransform transform(
      utm31n(), parse_crs("+proj=tmerc +lat_0=0 +lon_0=3 +k=1 +x_0=0 +y_0=0 +datum=WGS84"));
  ASSERT_TRUE(transform.affine());

  const std::array<double, 2> o = transform.apply(600000.0, 5800000.0);
  const std::array<double, 2> dx = transform.apply(601000.0, 5800000.0);
  const std::array<double, 2> dy = transform.apply(600000.0, 5801000.0);
  const std::array<double, 2> both = transform.apply(601000.0, 5801000.0);
  EXPECT_NEAR(both[0], dx[0] + dy[0] - o[0], 1e-6);
  EXPECT_NEAR(both[1], dx[1] + dy[1] - o[1], 1e-6);
}

TEST(GisCrs, TransformInvertsItself) {
  const CrsTransform transform(parse_crs("EPSG:4326"), utm31n());
  const std::array<double, 2> there = transform.apply(4.8952, 52.3702);
  const std::array<double, 2> back = transform.invert(there[0], there[1]);
  EXPECT_NEAR(back[0], 4.8952, 1e-9);
  EXPECT_NEAR(back[1], 52.3702, 1e-9);
}

// --- ★ The gate: one tokeniser, two callers, no disagreement ---------------

TEST(GisCrs, ParseCrsAndTmercOriginAgreeOnEveryTmercString) {
  // p7-s5's `tmerc_origin` and p7-s2's `parse_crs` read the same syntax through
  // the same tokeniser (core/src/road/proj_string.hpp). This asserts they stay
  // that way: two readings of one string that drift apart would show up as a
  // scene reporting one world origin while reprojecting imports to another.
  const std::vector<std::string> strings{
      "+proj=tmerc +lat_0=52.3702 +lon_0=4.8952 +k=1 +x_0=0 +y_0=0 +datum=WGS84 +units=m",
      "+proj=tmerc +lat_0=0 +lon_0=0 +datum=WGS84",
      "+proj=tmerc +lon_0=9 +lat_0=-33.5 +k=1 +datum=WGS84",
      // These are tmerc but NOT the origin family: scaled, or falsely eastered.
      "+proj=tmerc +lat_0=0 +lon_0=3 +k=0.9996 +x_0=500000 +y_0=0 +datum=WGS84",
      "+proj=tmerc +lat_0=0 +lon_0=3 +k=1 +x_0=100 +y_0=0 +datum=WGS84",
      // Not tmerc at all.
      "+proj=utm +zone=31 +datum=WGS84",
      "+proj=longlat +datum=WGS84",
      "",
  };

  for (const std::string& text : strings) {
    const Crs crs = parse_crs(text);
    const std::optional<std::array<double, 2>> origin = tmerc_origin(text);

    if (!origin.has_value()) {
      // tmerc_origin declines everything that is not unit-scale, un-shifted
      // tmerc. parse_crs may still recognise it (UTM, longlat) — what must
      // never happen is parse_crs reading a tmerc origin that tmerc_origin
      // refuses to name.
      const bool is_origin_family = crs.kind == CrsKind::TransverseMercator && crs.k_0 == 1.0 &&
                                    crs.x_0 == 0.0 && crs.y_0 == 0.0;
      EXPECT_FALSE(is_origin_family) << text;
      continue;
    }

    ASSERT_EQ(crs.kind, CrsKind::TransverseMercator) << text;
    EXPECT_DOUBLE_EQ(crs.lat_0, (*origin)[0]) << text;
    EXPECT_DOUBLE_EQ(crs.lon_0, (*origin)[1]) << text;
    EXPECT_DOUBLE_EQ(crs.k_0, 1.0) << text;
    EXPECT_DOUBLE_EQ(crs.x_0, 0.0) << text;
    EXPECT_DOUBLE_EQ(crs.y_0, 0.0) << text;
  }
}

TEST(GisCrs, DescribeNamesUtmZonesAndEchoesOpaqueText) {
  EXPECT_EQ(describe_crs(utm31n()), "UTM zone 31N");
  EXPECT_EQ(describe_crs(parse_crs("EPSG:32731")), "UTM zone 31S");
  EXPECT_EQ(describe_crs(parse_crs("EPSG:4326")), "WGS 84 geographic (lon/lat)");
  EXPECT_EQ(describe_crs(scene_crs(GeoReference{})), "none (local Cartesian)");
  EXPECT_NE(describe_crs(parse_crs("+proj=lcc +datum=NAD27")).find("+proj=lcc"), std::string::npos);
}
