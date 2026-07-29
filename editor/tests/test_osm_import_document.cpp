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

// The editor half of the OSM import (p7-s4, #244).
//
// The menu handler itself is a QFileDialog and four calls, but two of its
// properties are worth pinning because they are what a user actually
// experiences: an entire district is ONE entry on the undo stack, and the
// import's compromises reach the Diagnostics dock rather than only the log.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osm/import.hpp"
#include "roadmaker/road/georeference.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <filesystem>
#include <string>
#include <utility>

#include "document/document.hpp"

namespace {

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(RM_OSM_FIXTURES_DIR) / name;
}

void georeference(roadmaker::editor::Document& document) {
  roadmaker::GeoReference geo;
  geo.projection = roadmaker::tmerc_projection(52.3702, 4.8952).value_or(std::string{});
  EXPECT_TRUE(document.push_command(roadmaker::edit::set_georeference(document.network(), geo)));
}

std::size_t road_count(const roadmaker::RoadNetwork& network) {
  std::size_t count = 0;
  network.for_each_road([&count](roadmaker::RoadId, const roadmaker::Road&) { ++count; });
  return count;
}

} // namespace

TEST(OsmImportDocument, AWholeDistrictIsOneUndoEntry) {
  roadmaker::editor::Document document;
  georeference(document);
  const int before_import = document.undo_stack()->count();

  auto import = roadmaker::osm::prepare_osm_import(document.network(), fixture("topology.osm"));
  ASSERT_TRUE(import.has_value()) << (import ? "" : import.error().message);
  ASSERT_TRUE(document.push_command(std::move(import->command)));

  // ONE entry, not one per road. A district that took hundreds of presses to
  // remove would not be one edit however many roads it made.
  EXPECT_EQ(document.undo_stack()->count(), before_import + 1);
  EXPECT_GT(road_count(document.network()), 0U);

  document.undo_stack()->undo();
  EXPECT_EQ(road_count(document.network()), 0U);
  document.undo_stack()->redo();
  EXPECT_GT(road_count(document.network()), 0U);
}

TEST(OsmImportDocument, TheImportsCompromisesReachTheDiagnosticsDock) {
  roadmaker::editor::Document document;
  georeference(document);

  QSignalSpy spy(&document, &roadmaker::editor::Document::diagnostics_changed);
  const std::size_t before = document.diagnostics().size();

  auto import = roadmaker::osm::prepare_osm_import(document.network(), fixture("district.osm"));
  ASSERT_TRUE(import.has_value());
  ASSERT_FALSE(import->diagnostics.empty()) << "the district drops ways, so it must warn";

  document.report_diagnostics(import->diagnostics);

  // The dock is the only place a user sees WHICH way was dropped and why —
  // validate_network only ever sees the result, and could not reconstruct
  // "way 28374501 was simplified from 214 nodes to 61" from it.
  EXPECT_GT(document.diagnostics().size(), before);
  EXPECT_EQ(spy.count(), 1);
}

TEST(OsmImportDocument, AnUngeoreferencedSceneRefusesAndStaysUngeoreferenced) {
  roadmaker::editor::Document document; // no georeference

  const auto refused =
      roadmaker::osm::prepare_osm_import(document.network(), fixture("topology.osm"));
  ASSERT_FALSE(refused.has_value());

  // The refusal must be the one the GIS and lidar importers give, because it
  // is literally the same call — three importers disagreeing about one scene
  // would be worse than any of them refusing it.
  EXPECT_NE(refused.error().message.find("georeference"), std::string::npos)
      << refused.error().message;

  // And the importer must not have "helpfully" picked an origin: that would
  // give the scene a projection the user never chose.
  EXPECT_TRUE(document.network().georeference().projection.empty());
  EXPECT_EQ(road_count(document.network()), 0U);
}

TEST(OsmImportDocument, ReportDiagnosticsIsSilentWhenThereIsNothingToSay) {
  // The inverse, again: a dock that gains an empty group on every clean import
  // teaches a user to ignore it.
  roadmaker::editor::Document document;
  QSignalSpy spy(&document, &roadmaker::editor::Document::diagnostics_changed);
  const std::size_t before = document.diagnostics().size();

  document.report_diagnostics({});

  EXPECT_EQ(document.diagnostics().size(), before);
  EXPECT_EQ(spy.count(), 0);
}
