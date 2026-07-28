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

// The world georeference (p7-s5, #324): the §8.5 model, the §8.5 offset affine,
// the Transverse Mercator construction that lets this ship without PROJ, and
// the reader/writer header dispatch that #453 (fmt-f1) hands over here.
//
// WHY THE FIXTURES ARE BUILT IN THIS FILE. When this sprint started, NOT ONE
// committed .xodr — sample or fuzz corpus — carried a <geoReference>. That is
// the same shape as the omission #390 records: an assertion about data no
// fixture contains passes vacuously forever. The documents below are written
// here so every claim has something to bite on, and the sprint also adds
// corpus seeds so the fuzzer sees the element at all.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/io/export_preview.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/rules.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <string_view>

using roadmaker::GeoOffset;
using roadmaker::GeoReference;
using roadmaker::RoadNetwork;

namespace {

/// A minimal but VALID document — one straight road, so the network has plan
/// bounds and the writer has a bounding box to derive. `header_body` is spliced
/// into <header>, which is the whole point of every case here.
std::string document_with_header(std::string_view header_attributes,
                                 std::string_view header_children) {
  return std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenDRIVE>
  <header revMajor="1" revMinor="8" name="geo-test" )") +
         std::string(header_attributes) + R"(>
)" + std::string(header_children) +
         R"(  </header>
  <road name="r" length="100" id="1" junction="-1">
    <planView>
      <geometry s="0" x="0" y="0" hdg="0" length="100"><line/></geometry>
    </planView>
    <lanes>
      <laneSection s="0">
        <center>
          <lane id="0" type="none" level="false"/>
        </center>
        <right>
          <lane id="-1" type="driving" level="false">
            <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
          </lane>
        </right>
      </laneSection>
    </lanes>
  </road>
</OpenDRIVE>
)";
}

roadmaker::XodrParseResult parse(const std::string& xml) {
  auto parsed = roadmaker::parse_xodr(xml, "geo-test");
  EXPECT_TRUE(parsed.has_value());
  return std::move(*parsed);
}

bool has_rule(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view rule) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const roadmaker::Diagnostic& d) {
    return d.rule_id == rule;
  });
}

bool mentions(const std::vector<roadmaker::Diagnostic>& diagnostics, std::string_view needle) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const roadmaker::Diagnostic& d) {
    return d.message.find(needle) != std::string::npos;
  });
}

/// The <header ...> start tag of a written document.
///
/// A test asserting on header content CANNOT grep the whole file: `west=` would
/// also be satisfied by nothing today but `name=` matches <road name=> and
/// `length=` matches <road length=>. #429 lost two tests to exactly that, so
/// the slice is taken once, here.
std::string header_tag(const std::string& xml) {
  const std::size_t start = xml.find("<header");
  EXPECT_NE(start, std::string::npos);
  const std::size_t end = xml.find('>', start);
  EXPECT_NE(end, std::string::npos);
  return xml.substr(start, end - start + 1);
}

} // namespace

// --- the §8.5 offset affine -------------------------------------------------

TEST(GeoOffset, AnAbsentOffsetAndAZeroOffsetAgree) {
  EXPECT_TRUE(GeoOffset{}.identity());
  EXPECT_FALSE((GeoOffset{.x = 1.0}).identity());
  EXPECT_FALSE((GeoOffset{.hdg = 0.5}).identity());
}

TEST(GeoOffset, TranslationMatchesTheSpecFormulas) {
  // §8.5, the no-heading simplification: xWorld = xODR + xOffset, and so on.
  const GeoOffset offset{.x = 100.0, .y = -50.0, .z = 7.0, .hdg = 0.0};
  const auto world = roadmaker::geo_to_world(offset, 10.0, 20.0, 1.0);
  EXPECT_DOUBLE_EQ(world[0], 110.0);
  EXPECT_DOUBLE_EQ(world[1], -30.0);
  EXPECT_DOUBLE_EQ(world[2], 8.0);
}

TEST(GeoOffset, RotationMatchesTheSpecFormulas) {
  // xWorld = xODR*cos(hdg) - yODR*sin(hdg) + xOffset, and the y twin.
  const GeoOffset offset{.x = 0.0, .y = 0.0, .z = 0.0, .hdg = std::numbers::pi / 2.0};
  const auto world = roadmaker::geo_to_world(offset, 1.0, 0.0, 0.0);
  EXPECT_NEAR(world[0], 0.0, 1e-12);
  EXPECT_NEAR(world[1], 1.0, 1e-12);
}

TEST(GeoOffset, LocalAndWorldAreExactInverses) {
  // The pair has to compose to the identity for EVERY component, including the
  // rotation — an inverse that only undoes the translation looks right in the
  // common no-heading case and is wrong exactly where it matters.
  const GeoOffset offset{.x = 1234.5, .y = -987.25, .z = 3.5, .hdg = 0.7853981633974483};
  for (const auto& point : {std::array<double, 3>{0.0, 0.0, 0.0},
                            std::array<double, 3>{10.0, -20.0, 5.0},
                            std::array<double, 3>{-1e5, 2.5e4, -12.0}}) {
    const auto world = roadmaker::geo_to_world(offset, point[0], point[1], point[2]);
    const auto back = roadmaker::geo_to_local(offset, world[0], world[1], world[2]);
    EXPECT_NEAR(back[0], point[0], 1e-9);
    EXPECT_NEAR(back[1], point[1], 1e-9);
    EXPECT_NEAR(back[2], point[2], 1e-9);
  }
}

// --- the Transverse Mercator construction -----------------------------------

TEST(TmercProjection, PlacesTheProjectionOriginAtTheSceneOrigin) {
  const auto proj = roadmaker::tmerc_projection(37.7749, -122.4194);
  ASSERT_TRUE(proj.has_value());
  // +k=1 with no false easting or northing is what makes local coordinates and
  // projected coordinates the same numbers — the property the whole PROJ-free
  // design rests on. If any of these three drift, the claim is false.
  EXPECT_NE(proj->find("+proj=tmerc"), std::string::npos);
  EXPECT_NE(proj->find("+k=1"), std::string::npos);
  EXPECT_NE(proj->find("+x_0=0"), std::string::npos);
  EXPECT_NE(proj->find("+y_0=0"), std::string::npos);
}

TEST(TmercProjection, TheOriginSurvivesTheStringExactly) {
  // Not "close to": bit-for-bit. The UI reads the origin back out of this
  // string, so a lossy format would show the user a different place than the
  // file records. fmt-s1 (#325) lost a day to the fixed-precision version of
  // this mistake, where nine digits turned 0.8 into 0.800000012.
  for (const double lat : {0.0, 37.7749, -33.8688, 89.9999999, -0.000001}) {
    for (const double lon : {0.0, -122.4194, 151.2093, 179.9999999}) {
      const auto proj = roadmaker::tmerc_projection(lat, lon);
      ASSERT_TRUE(proj.has_value());
      const auto origin = roadmaker::tmerc_origin(*proj);
      ASSERT_TRUE(origin.has_value()) << *proj;
      EXPECT_EQ((*origin)[0], lat) << *proj;
      EXPECT_EQ((*origin)[1], lon) << *proj;
    }
  }
}

TEST(TmercProjection, RejectsAnglesOffTheGlobe) {
  EXPECT_FALSE(roadmaker::tmerc_projection(90.001, 0.0).has_value());
  EXPECT_FALSE(roadmaker::tmerc_projection(-90.001, 0.0).has_value());
  EXPECT_FALSE(roadmaker::tmerc_projection(0.0, 180.001).has_value());
  EXPECT_FALSE(roadmaker::tmerc_projection(std::nan(""), 0.0).has_value());
  EXPECT_FALSE(roadmaker::tmerc_projection(0.0, std::numeric_limits<double>::infinity()).has_value());
}

TEST(TmercOrigin, DeclinesEveryProjectionItCannotResolve) {
  // The honest half of shipping without PROJ: a CRS we cannot read must say so
  // rather than guess. A UTM zone has a false easting of 500 km, so reporting
  // its +lat_0 as the scene origin would be wrong by exactly that much.
  EXPECT_FALSE(
      roadmaker::tmerc_origin("+proj=utm +zone=32 +ellps=GRS80 +units=m +no_defs").has_value());
  EXPECT_FALSE(roadmaker::tmerc_origin("+proj=merc +lat_0=10 +lon_0=20").has_value());
  EXPECT_FALSE(roadmaker::tmerc_origin("PROJCS[\"WGS 84 / UTM zone 32N\"]").has_value());
  EXPECT_FALSE(roadmaker::tmerc_origin("").has_value());
  // A tmerc that DOES shift its grid is still unresolvable — local and
  // projected coordinates no longer coincide.
  EXPECT_FALSE(
      roadmaker::tmerc_origin("+proj=tmerc +lat_0=1 +lon_0=2 +x_0=500000 +y_0=0").has_value());
  EXPECT_FALSE(roadmaker::tmerc_origin("+proj=tmerc +lat_0=1 +lon_0=2 +k=0.9996").has_value());
}

TEST(TmercOrigin, ReadsAStringAnotherToolReformatted) {
  // Parameter order and spacing are not part of a PROJ string's meaning, so a
  // file that went through someone else's writer must still read.
  const auto origin =
      roadmaker::tmerc_origin("  +units=m   +lon_0=9.5 +no_defs +proj=tmerc\t+lat_0=47.25 ");
  ASSERT_TRUE(origin.has_value());
  EXPECT_DOUBLE_EQ((*origin)[0], 47.25);
  EXPECT_DOUBLE_EQ((*origin)[1], 9.5);
}

// --- reading <header> -------------------------------------------------------

TEST(HeaderGeoReference, ACdataProjectionStringIsRead) {
  const auto parsed = parse(document_with_header(
      "", "    <geoReference><![CDATA[+proj=utm +zone=32 +datum=WGS84]]></geoReference>\n"));
  EXPECT_EQ(parsed.network.georeference().projection, "+proj=utm +zone=32 +datum=WGS84");
  EXPECT_FALSE(parsed.network.georeference().empty());
}

TEST(HeaderGeoReference, APlainTextProjectionStringIsAlsoRead) {
  // §8.5 says the string "shall be marked as CDATA", but a file that omits the
  // wrapper is still readable and refusing it would lose data for nothing.
  const auto parsed =
      parse(document_with_header("", "    <geoReference>+proj=longlat</geoReference>\n"));
  EXPECT_EQ(parsed.network.georeference().projection, "+proj=longlat");
}

TEST(HeaderGeoReference, SurroundingWhitespaceIsLayoutNotData) {
  const auto parsed = parse(document_with_header(
      "", "    <geoReference>\n      <![CDATA[+proj=tmerc]]>\n    </geoReference>\n"));
  EXPECT_EQ(parsed.network.georeference().projection, "+proj=tmerc");
}

TEST(HeaderGeoReference, ASecondDefinitionIsRefusedByRule) {
  const auto parsed = parse(
      document_with_header("",
                           "    <geoReference><![CDATA[+proj=tmerc +lat_0=1]]></geoReference>\n"
                           "    <geoReference><![CDATA[+proj=utm +zone=9]]></geoReference>\n"));
  // First wins — anything else makes the projection depend on document order.
  EXPECT_EQ(parsed.network.georeference().projection, "+proj=tmerc +lat_0=1");
  EXPECT_TRUE(has_rule(parsed.diagnostics, roadmaker::rules::kHeaderMaxOneProj));
}

TEST(HeaderOffset, AllFourAttributesAreRead) {
  const auto parsed =
      parse(document_with_header("", "    <offset x=\"10\" y=\"-20\" z=\"3\" hdg=\"0.5\"/>\n"));
  ASSERT_TRUE(parsed.network.georeference().offset.has_value());
  const GeoOffset& offset = *parsed.network.georeference().offset;
  EXPECT_DOUBLE_EQ(offset.x, 10.0);
  EXPECT_DOUBLE_EQ(offset.y, -20.0);
  EXPECT_DOUBLE_EQ(offset.z, 3.0);
  EXPECT_DOUBLE_EQ(offset.hdg, 0.5);
}

TEST(HeaderOffset, APartialOffsetIsReportedAndDropped) {
  // Table 17 marks all four required. Completing a partial element with zeros
  // would invent a claim about where the dataset sits that nobody wrote.
  const auto parsed = parse(document_with_header("", "    <offset x=\"10\" y=\"-20\"/>\n"));
  EXPECT_FALSE(parsed.network.georeference().offset.has_value());
  EXPECT_TRUE(mentions(parsed.diagnostics, "all four"));
}

TEST(Header, UnmodeledChildrenAndAttributesSurvive) {
  // The fmt-f1 (#453) row this sprint discharges: before it, a georeferenced
  // foreign file lost its projection string AND its <license> with no
  // diagnostic anywhere.
  const auto parsed = parse(document_with_header(
      R"(date="2026-07-28T10:00:00" version="1.00")",
      "    <geoReference><![CDATA[+proj=tmerc +lat_0=1 +lon_0=2]]></geoReference>\n"
      "    <license name=\"CC BY 4.0\" spdxid=\"CC-BY-4.0\"/>\n"
      "    <userData code=\"acme:survey\">crew 7</userData>\n"));

  const roadmaker::RawXml& preserved = parsed.network.preserved_header();
  EXPECT_EQ(preserved.attributes.size(), 2U);
  EXPECT_EQ(preserved.attributes[0].first, "date");
  EXPECT_EQ(preserved.attributes[1].first, "version");
  ASSERT_EQ(preserved.children.size(), 2U);
  EXPECT_NE(preserved.children[0].find("<license"), std::string::npos);
  EXPECT_NE(preserved.children[1].find("acme:survey"), std::string::npos);
  // The modeled children are modeled, not preserved — otherwise they would be
  // written twice.
  for (const std::string& child : preserved.children) {
    EXPECT_EQ(child.find("geoReference"), std::string::npos);
  }
}

TEST(Header, TheWriterOwnedAttributesAreNotPreserved) {
  // Carrying an input's bounding box forward would turn it into a lie the
  // moment anything moved, so those four are read and discarded.
  const auto parsed = parse(document_with_header(
      R"(north="1" south="2" east="3" west="4" vendor="SomeoneElse")", ""));
  for (const auto& [name, value] : parsed.network.preserved_header().attributes) {
    EXPECT_NE(name, "north");
    EXPECT_NE(name, "south");
    EXPECT_NE(name, "east");
    EXPECT_NE(name, "west");
    EXPECT_NE(name, "vendor");
  }
}

// --- writing <header> -------------------------------------------------------

TEST(HeaderWriter, TheBoundingBoxDescribesTheNetworkBeingWritten) {
  const auto parsed = parse(document_with_header("", ""));
  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());
  const std::string tag = header_tag(*xml);
  EXPECT_NE(tag.find("west="), std::string::npos) << tag;
  EXPECT_NE(tag.find("east="), std::string::npos) << tag;
  EXPECT_NE(tag.find("north="), std::string::npos) << tag;
  EXPECT_NE(tag.find("south="), std::string::npos) << tag;

  // The 100 m road runs along +x from the origin, grown by its half width, so
  // the box has to reach past 100 in x and stay small in y. Asserting the
  // ORIENTATION rather than exact numbers keeps this from re-implementing
  // network_plan_bounds.
  const auto bounds = roadmaker::network_plan_bounds(parsed.network);
  ASSERT_TRUE(bounds.has_value());
  EXPECT_GT((*bounds)[2], 100.0);
  EXPECT_LT((*bounds)[3] - (*bounds)[1], 20.0);
}

TEST(HeaderWriter, AnEmptyNetworkStatesNoExtent) {
  // Table 8 marks all four optional, and a scene with no geometry has no
  // extent to state — inventing a zero box would claim the dataset sits at the
  // origin with no size.
  const RoadNetwork empty;
  const auto xml = roadmaker::write_xodr(empty, "geo-test");
  ASSERT_TRUE(xml.has_value());
  const std::string tag = header_tag(*xml);
  EXPECT_EQ(tag.find("west="), std::string::npos) << tag;
  EXPECT_EQ(tag.find("north="), std::string::npos) << tag;
}

TEST(HeaderWriter, TheProjectionStringIsWrittenAsCdata) {
  // §8.5: the string "shall be marked as CDATA, because it may contain
  // characters that interfere with the XML syntax". This is the only CDATA
  // node RoadMaker writes anywhere, so nothing else guards the behaviour.
  auto parsed = parse(document_with_header("", ""));
  parsed.network.set_georeference(GeoReference{.projection = "+proj=tmerc +lat_0=1 <&>"});
  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());
  EXPECT_NE(xml->find("<geoReference><![CDATA[+proj=tmerc +lat_0=1 <&>]]></geoReference>"),
            std::string::npos)
      << *xml;
  // And the whole point of CDATA: the angle brackets survive unescaped, so a
  // consumer gets the string the author wrote.
  const auto reparsed = parse(*xml);
  EXPECT_EQ(reparsed.network.georeference().projection, "+proj=tmerc +lat_0=1 <&>");
}

TEST(HeaderWriter, AnIdentityOffsetIsNotWritten) {
  auto parsed = parse(document_with_header("", ""));
  parsed.network.set_georeference(GeoReference{.projection = "+proj=tmerc", .offset = GeoOffset{}});
  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(xml->find("<offset"), std::string::npos) << *xml;
}

TEST(HeaderWriter, AnEmptyGeoReferenceWritesNothingAtAll) {
  // The compatibility floor: a scene that never touched georeferencing has to
  // produce the same header children it always did — none.
  const auto parsed = parse(document_with_header("", ""));
  EXPECT_TRUE(parsed.network.georeference().empty());
  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(xml->find("<geoReference"), std::string::npos);
  EXPECT_EQ(xml->find("<offset"), std::string::npos);
}

// --- round trips ------------------------------------------------------------

TEST(HeaderRoundTrip, TheGeoreferenceAndTheHeaderRemaindersAreAFixedPoint) {
  const std::string source = document_with_header(
      R"(date="2026-07-28T10:00:00" version="1.00")",
      "    <geoReference><![CDATA[+proj=tmerc +lat_0=37.7749 +lon_0=-122.4194 +k=1 +x_0=0 "
      "+y_0=0 +datum=WGS84 +units=m +no_defs]]></geoReference>\n"
      "    <offset x=\"1000\" y=\"-2000\" z=\"5\" hdg=\"0\"/>\n"
      "    <license name=\"CC BY 4.0\" spdxid=\"CC-BY-4.0\"/>\n");

  const auto first = parse(source);
  const auto once = roadmaker::write_xodr(first.network, "geo-test");
  ASSERT_TRUE(once.has_value());

  const auto second = parse(*once);
  const auto twice = roadmaker::write_xodr(second.network, "geo-test");
  ASSERT_TRUE(twice.has_value());

  EXPECT_EQ(*once, *twice);
  EXPECT_EQ(second.network.georeference(), first.network.georeference());
  EXPECT_EQ(second.network.preserved_header(), first.network.preserved_header());

  // Non-vacuity: the fixed point has to be a fixed point of something. If the
  // georeference were being dropped, both writes would agree on nothing.
  EXPECT_FALSE(first.network.georeference().empty());
  ASSERT_TRUE(first.network.georeference().offset.has_value());
  EXPECT_DOUBLE_EQ(first.network.georeference().offset->x, 1000.0);
  EXPECT_FALSE(first.network.preserved_header().empty());
}

TEST(HeaderRoundTrip, AGeneratedOriginSurvivesTheFile) {
  // The end-to-end claim the editor makes: set an origin, save, reload, and the
  // same latitude and longitude come back.
  const auto proj = roadmaker::tmerc_projection(48.858844, 2.294351);
  ASSERT_TRUE(proj.has_value());

  auto parsed = parse(document_with_header("", ""));
  parsed.network.set_georeference(GeoReference{.projection = *proj});
  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());

  const auto reloaded = parse(*xml);
  const auto origin = roadmaker::tmerc_origin(reloaded.network.georeference().projection);
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ((*origin)[0], 48.858844);
  EXPECT_EQ((*origin)[1], 2.294351);
}

// --- the centred-coordinates advisory ---------------------------------------

TEST(GeoReferenceValidation, AFarFlungNetworkWithNoOffsetIsAdvised) {
  // The road sits at UTM-like coordinates, where a consumer holding positions
  // as float starts losing centimetres — the failure §8.5 describes.
  std::string far = document_with_header("", "");
  const std::size_t geometry = far.find(R"(x="0" y="0")");
  ASSERT_NE(geometry, std::string::npos);
  far.replace(geometry, std::string_view(R"(x="0" y="0")").size(), R"(x="500000" y="4000000")");
  auto parsed = parse(far);

  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_TRUE(has_rule(findings, roadmaker::rules::kHeaderOffsetCenteredCoords));

  // Declaring an offset is the author saying they know — the rule exists to
  // prompt exactly that, so it must stop prompting once they have.
  parsed.network.set_georeference(
      GeoReference{.offset = GeoOffset{.x = -500000.0, .y = -4000000.0}});
  const auto quiet = roadmaker::validate_network(parsed.network);
  EXPECT_FALSE(has_rule(quiet, roadmaker::rules::kHeaderOffsetCenteredCoords));
}

TEST(GeoReferenceValidation, ANetworkNearTheOriginIsNotAdvised) {
  const auto parsed = parse(document_with_header("", ""));
  const auto findings = roadmaker::validate_network(parsed.network);
  EXPECT_FALSE(has_rule(findings, roadmaker::rules::kHeaderOffsetCenteredCoords));
}

// --- the export preview's header read-out -----------------------------------

TEST(XodrPreviewHeader, ReportsTheGeoreferenceTheFileWouldCarry) {
  auto parsed = parse(document_with_header("", ""));
  parsed.network.set_georeference(
      GeoReference{.projection = "+proj=tmerc +lat_0=1 +lon_0=2",
                   .offset = GeoOffset{.x = 10.0, .y = 20.0, .z = 1.0, .hdg = 0.25}});

  const roadmaker::XodrPreview preview = roadmaker::preview_xodr_export(parsed.network, "geo-test");
  ASSERT_TRUE(preview.would_write);
  EXPECT_EQ(preview.header.geo_reference, "+proj=tmerc +lat_0=1 +lon_0=2");
  ASSERT_TRUE(preview.header.offset.has_value());
  EXPECT_DOUBLE_EQ(preview.header.offset->x, 10.0);
  EXPECT_DOUBLE_EQ(preview.header.offset->hdg, 0.25);
  ASSERT_TRUE(preview.header.bounds.has_value());
  EXPECT_GT((*preview.header.bounds)[2], 100.0); // east, past the 100 m road
}

TEST(XodrPreviewHeader, ReportsAbsenceRatherThanADefault) {
  // The preview has to distinguish "no projection" from "some projection we
  // failed to read" — an empty string is the honest report of the first.
  const auto parsed = parse(document_with_header("", ""));
  const roadmaker::XodrPreview preview = roadmaker::preview_xodr_export(parsed.network, "geo-test");
  EXPECT_TRUE(preview.header.geo_reference.empty());
  EXPECT_FALSE(preview.header.offset.has_value());
}

TEST(XodrPreviewHeader, AgreesWithTheBytesEvenWhenTheModelWouldNot) {
  // The reason this is read out of `xml` rather than off the RoadNetwork: the
  // network HAS an offset here, and the file will not carry one, because an
  // identity offset is not written. A model-derived read-out would report an
  // offset that is not in the file.
  auto parsed = parse(document_with_header("", ""));
  parsed.network.set_georeference(GeoReference{.projection = "+proj=tmerc", .offset = GeoOffset{}});
  ASSERT_TRUE(parsed.network.georeference().offset.has_value());

  const roadmaker::XodrPreview preview = roadmaker::preview_xodr_export(parsed.network, "geo-test");
  EXPECT_FALSE(preview.header.offset.has_value());
  EXPECT_EQ(preview.xml.find("<offset"), std::string::npos);
}

// --- the edit command -------------------------------------------------------

TEST(SetGeoReferenceCommand, ApplyThenRevertLeavesTheFileByteIdentical) {
  // The command-layer invariant (docs/design/m2/01): apply→revert must leave
  // write_xodr byte-identical. For this datum that means setting a
  // georeference and undoing it re-emits the header exactly as it was.
  auto parsed = parse(document_with_header("", ""));
  const auto before = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(before.has_value());

  auto command = roadmaker::edit::set_georeference(
      parsed.network,
      GeoReference{.projection = "+proj=tmerc +lat_0=1 +lon_0=2",
                   .offset = GeoOffset{.x = 10.0, .y = 20.0}});
  ASSERT_NE(command, nullptr);

  ASSERT_TRUE(command->apply(parsed.network).has_value());
  const auto during = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(during.has_value());
  // Non-vacuity: if the command did nothing, the byte-identity below would
  // hold for the wrong reason.
  EXPECT_NE(*during, *before);

  ASSERT_TRUE(command->revert(parsed.network).has_value());
  const auto after = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, *before);
}

TEST(SetGeoReferenceCommand, ClearingIsSettingTheEmptyValue) {
  auto parsed = parse(document_with_header(
      "", "    <geoReference><![CDATA[+proj=tmerc +lat_0=1]]></geoReference>\n"));
  ASSERT_FALSE(parsed.network.georeference().empty());

  auto command = roadmaker::edit::set_georeference(parsed.network, GeoReference{});
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(command->apply(parsed.network).has_value());
  EXPECT_TRUE(parsed.network.georeference().empty());

  const auto xml = roadmaker::write_xodr(parsed.network, "geo-test");
  ASSERT_TRUE(xml.has_value());
  EXPECT_EQ(xml->find("<geoReference"), std::string::npos);
}

TEST(SetGeoReferenceCommand, ChangesNoMeshChannel) {
  // The empty DirtySet is a claim, so it gets an assertion. A georeference
  // states how coordinates relate to the earth; it moves nothing.
  const auto parsed = parse(document_with_header("", ""));
  auto command =
      roadmaker::edit::set_georeference(parsed.network, GeoReference{.projection = "+proj=tmerc"});
  ASSERT_NE(command, nullptr);
  const roadmaker::edit::DirtySet dirty = command->dirty();
  EXPECT_TRUE(dirty.roads.empty());
  EXPECT_TRUE(dirty.junctions.empty());
  EXPECT_TRUE(dirty.objects.empty());
  EXPECT_TRUE(dirty.surfaces.empty());
  EXPECT_FALSE(dirty.terrain);
  EXPECT_FALSE(dirty.topology);
}

TEST(SetGeoReferenceCommand, RejectsBadInputWithoutMutating) {
  auto parsed = parse(document_with_header("", ""));
  const GeoReference original = parsed.network.georeference();

  const auto rejects = [&](GeoReference geo) {
    auto command = roadmaker::edit::set_georeference(parsed.network, std::move(geo));
    ASSERT_NE(command, nullptr);
    EXPECT_FALSE(command->apply(parsed.network).has_value());
    EXPECT_EQ(parsed.network.georeference(), original);
  };
  rejects(GeoReference{.projection = "   \t\n  "});
  rejects(GeoReference{.offset = GeoOffset{.x = std::nan("")}});
  rejects(GeoReference{.offset = GeoOffset{.hdg = std::numeric_limits<double>::infinity()}});
  rejects(GeoReference{}); // a no-op is not an edit
}
