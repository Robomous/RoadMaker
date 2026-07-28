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

// Prop obstruction in the editor (cascade-s4, #464): a move that drives a prop
// into a road REPORTS it and changes nothing, the report reaches the user, and
// the offered fix is one undoable command the user has to ask for. No modal —
// cascade-s1 removed the last pre-flight one because a dialog opened mid-drag
// swallows the mouse-release.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/prop_obstructions.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <array>
#include <string>

#include "document/document.hpp"

using roadmaker::LaneProfile;
using roadmaker::Object;
using roadmaker::ObjectId;
using roadmaker::ObjectType;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;

namespace {

/// Document::network() is const, so everything an editor test authors goes
/// through the command layer — which is the point: this is the same path the
/// product takes.
RoadId lay(Document& document, const char* name, Waypoint a, Waypoint b) {
  const bool ok =
      document
          .push_command(roadmaker::edit::create_road({a, b}, LaneProfile::two_lane_default(), name))
          .has_value();
  EXPECT_TRUE(ok);
  // The newest road: for_each_road walks slots ascending, so the last one seen
  // is the one just created. create_road's third argument is the display NAME,
  // not the OpenDRIVE id.
  RoadId road;
  document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
  return road;
}

ObjectId plant(Document& document, RoadId road, double s, double t) {
  Object object;
  object.odr_id = "1";
  object.name = "tree_pine";
  object.type = ObjectType::Tree;
  object.s = s;
  object.t = t;
  object.radius = 1.5;
  object.height = 4.0;
  EXPECT_TRUE(document.push_command(roadmaker::edit::add_object(document.network(), road, object))
                  .has_value());
  ObjectId id;
  document.network().for_each_object(
      [&](ObjectId oid, const Object& placed) { id = placed.odr_id == "1" ? oid : id; });
  return id;
}

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  EXPECT_TRUE(text.has_value());
  return text.has_value() ? *text : std::string{};
}

} // namespace

TEST(PropObstructionDocument, AMoveThatObstructsAPropTellsTheUserAndFixesNothing) {
  Document document;
  const RoadId anchor = lay(document, "anchor", {-50, 40}, {50, 40});
  lay(document, "crossed", {-50, 0}, {50, 0});
  const ObjectId prop = plant(document, anchor, 50.0, -20.0); // world (0, 20): clear
  ASSERT_TRUE(roadmaker::find_prop_obstructions(document.network()).empty());

  QSignalSpy obstructed(&document, &Document::props_obstructed);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::translate_road(document.network(), anchor, 0.0, -20.0))
          .has_value());

  EXPECT_EQ(obstructed.count(), 1) << "the user has to be told the tree is now in the road";
  EXPECT_EQ(roadmaker::find_prop_obstructions(document.network()).size(), 1U)
      << "and the move must NOT have quietly corrected it";
  EXPECT_NE(document.network().object(prop), nullptr);
}

TEST(PropObstructionDocument, TheOfferedRelocationIsOneUndoEntryAndUndoIsByteIdentical) {
  Document document;
  const RoadId anchor = lay(document, "anchor", {-50, 40}, {50, 40});
  lay(document, "crossed", {-50, 0}, {50, 0});
  plant(document, anchor, 50.0, -20.0);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::translate_road(document.network(), anchor, 0.0, -20.0))
          .has_value());
  ASSERT_FALSE(roadmaker::find_prop_obstructions(document.network()).empty());

  const std::string before = xodr(document);
  const int entries = document.undo_stack()->count();
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::relocate_obstructed_props(document.network()))
          .has_value());

  EXPECT_EQ(document.undo_stack()->count(), entries + 1) << "exactly one entry, however many props";
  EXPECT_TRUE(roadmaker::find_prop_obstructions(document.network()).empty());

  document.undo_stack()->undo();
  EXPECT_EQ(xodr(document), before);
}

TEST(PropObstructionDocument, RelocatingWithNothingObstructedIsARefusalNotAnEmptyEntry) {
  Document document;
  const RoadId anchor = lay(document, "anchor", {-50, 40}, {50, 40});
  lay(document, "crossed", {-50, 0}, {50, 0});
  plant(document, anchor, 50.0, -20.0);

  const int entries = document.undo_stack()->count();
  EXPECT_FALSE(
      document.push_command(roadmaker::edit::relocate_obstructed_props(document.network()))
          .has_value());
  EXPECT_EQ(document.undo_stack()->count(), entries) << "a refusal leaves the stack alone";
}

/// A prop that was already obstructed does not generate a fresh warning every
/// time an unrelated road is nudged — the report would be unusable otherwise.
TEST(PropObstructionDocument, AnAlreadyObstructedPropDoesNotWarnAgainOnEveryNudge) {
  Document document;
  const RoadId anchor = lay(document, "anchor", {-50, 40}, {50, 40});
  lay(document, "crossed", {-50, 0}, {50, 0});
  plant(document, anchor, 50.0, -40.0); // world (0, 0): obstructed from the start
  ASSERT_EQ(roadmaker::find_prop_obstructions(document.network()).size(), 1U);

  QSignalSpy obstructed(&document, &Document::props_obstructed);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::translate_road(document.network(), anchor, 0.01, 0.0))
          .has_value());
  EXPECT_EQ(obstructed.count(), 0);
}

TEST(PropObstructionDocument, ReanchoringKeepsThePropWhereItIsAndUndoRestoresTheOwner) {
  Document document;
  const RoadId anchor = lay(document, "anchor", {-50, 40}, {50, 40});
  const RoadId crossed = lay(document, "crossed", {-50, 0}, {50, 0});
  const ObjectId prop = plant(document, anchor, 50.0, -20.0);

  const std::string before = xodr(document);
  ASSERT_TRUE(
      document.push_command(roadmaker::edit::reanchor_object(document.network(), prop, crossed))
          .has_value());
  EXPECT_EQ(document.network().object(prop)->road, crossed);
  EXPECT_NE(document.network().object(prop), nullptr) << "the id survives, so the selection does";

  document.undo_stack()->undo();
  EXPECT_EQ(xodr(document), before);
  EXPECT_EQ(document.network().object(prop)->road, anchor);
}
