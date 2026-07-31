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

// The scenario half of Document (p8-s2, issue #246): a `.xosc` stem-matched
// beside its `.xodr`, mutated through the SAME undo stack as the map.
//
// THE THREE CONTRACTS THIS FILE EXISTS TO PIN, all of them things a
// field-by-field check would miss:
//
//   1. A scene with no scenario loads in SILENCE and saves NO `.xosc`. Every
//      pre-p8 project is that scene, and sprouting an empty file beside every
//      `.xodr` would be a visible regression for every existing user.
//   2. ONE undo stack. A scenario command and a map command interleave on
//      Document's QUndoStack, so undoing past a scenario edit reaches the map
//      edit before it. That is what "the undo history is intact after a mode
//      round-trip" (GW-6 step 1) actually means.
//   3. save -> reopen -> save is BYTE-IDENTICAL (GW-6 step 12), asserted on the
//      bytes of the file rather than on the parsed model.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// A one-road network, so a saved scene is a real scene.
void author_a_road(Document& document) {
  auto command = edit::create_road({Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = 100.0, .y = 0.0}},
                                   LaneProfile::two_lane_default(),
                                   "main");
  ASSERT_NE(command, nullptr);
  ASSERT_TRUE(document.push_command(std::move(command)).has_value());
}

osc::LanePosition lane_at(double s) {
  osc::LanePosition lane;
  lane.road_id = "1";
  lane.lane_id = "-1";
  lane.s = s;
  return lane;
}

/// Places one actor through the document's own command path. Every scenario
/// mutation in these tests goes through push_scenario_command — a test that
/// wrote `scenario_` directly would prove nothing about the undo stack.
void place_an_actor(Document& document, const char* name, double s = 20.0) {
  ASSERT_TRUE(
      document.push_scenario_command(osc::edit::set_logic_file(document.scenario(), "scene.xodr"))
          .has_value());
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), osc::make_actor(osc::ActorKind::Car, name), lane_at(s)))
                  .has_value());
}

} // namespace

TEST(ScenarioDocument, AFreshDocumentHasNoScenario) {
  Document document;
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty());
  EXPECT_FALSE(document.has_scenario());
  EXPECT_TRUE(document.scenario_path().empty()) << "an unsaved document has no scenario path";
}

TEST(ScenarioDocument, TheScenarioPathIsStemMatchedToTheScene) {
  // ADR-0014 §9: town.xodr pairs with town.xosc, beside it. Derived, never
  // stored — a stored copy could only ever disagree with the scene.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";

  Document document;
  author_a_road(document);
  ASSERT_TRUE(document.save(scene).has_value());

  EXPECT_EQ(document.scenario_path(), scene.parent_path() / "town.xosc");
}

TEST(ScenarioDocument, ASceneWithNoScenarioWritesNoXoscAtAll) {
  // ★ Every pre-p8 project is this scene. An empty .xosc beside every .xodr
  // would be a visible regression for every existing user, and the kind that
  // only shows up in a file listing.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene =
      std::filesystem::path(dir.path().toStdString()) / "plain.xodr";

  Document document;
  author_a_road(document);
  ASSERT_TRUE(document.save(scene).has_value());

  EXPECT_TRUE(std::filesystem::exists(scene));
  EXPECT_FALSE(std::filesystem::exists(scene.parent_path() / "plain.xosc"));
}

TEST(ScenarioDocument, AMissingScenarioLoadsInSilence) {
  // The scene_sidecar contract, applied to the second Layer-0 file: a missing
  // one is the COMMON case and must not be logged as a problem or reported as
  // a diagnostic.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene =
      std::filesystem::path(dir.path().toStdString()) / "plain.xodr";

  Document writer;
  author_a_road(writer);
  ASSERT_TRUE(writer.save(scene).has_value());

  Document reader;
  ASSERT_TRUE(reader.load(scene).has_value());
  EXPECT_FALSE(reader.has_scenario());
  for (const Diagnostic& finding : reader.diagnostics()) {
    EXPECT_EQ(finding.message.find("scenario"), std::string::npos)
        << "a missing scenario was reported: " << finding.message;
  }
}

TEST(ScenarioDocument, AnAuthoredScenarioSavesBesideTheSceneAndLoadsBack) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";

  Document writer;
  author_a_road(writer);
  place_an_actor(writer, "Car1");
  ASSERT_TRUE(writer.has_scenario());
  ASSERT_TRUE(writer.save(scene).has_value());
  ASSERT_TRUE(std::filesystem::exists(scene.parent_path() / "town.xosc"));

  Document reader;
  ASSERT_TRUE(reader.load(scene).has_value());
  ASSERT_EQ(reader.scenario().entities.scenario_objects.size(), 1U);
  EXPECT_EQ(reader.scenario().entities.scenario_objects[0].name, "Car1");
  EXPECT_TRUE(reader.has_scenario());
}

TEST(ScenarioDocument, SaveReopenSaveIsByteIdentical) {
  // GW-6 step 12, asserted on the BYTES. A model comparison would pass on a
  // round trip that reordered a vector or dropped an optional — both of which
  // change the file.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";
  const std::filesystem::path xosc = scene.parent_path() / "town.xosc";

  Document writer;
  author_a_road(writer);
  place_an_actor(writer, "Car1");
  ASSERT_TRUE(
      writer
          .push_scenario_command(osc::edit::set_entity_init_speed(writer.scenario(), "Car1", 13.89))
          .has_value());
  ASSERT_TRUE(writer.save(scene).has_value());
  const std::string first = read_bytes(xosc);
  ASSERT_FALSE(first.empty());

  Document reader;
  ASSERT_TRUE(reader.load(scene).has_value());
  ASSERT_TRUE(reader.save(scene).has_value());
  EXPECT_EQ(read_bytes(xosc), first);
}

TEST(ScenarioDocument, LoadingASceneReplacesTheScenarioRatherThanMergingIt) {
  // A stale actor surviving a load is the generational-id hazard's twin: the
  // name would resolve, so nothing downstream could detect it.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const auto root = std::filesystem::path(dir.path().toStdString());

  Document writer;
  author_a_road(writer);
  place_an_actor(writer, "Car1");
  ASSERT_TRUE(writer.save(root / "with.xodr").has_value());

  Document plain;
  author_a_road(plain);
  ASSERT_TRUE(plain.save(root / "without.xodr").has_value());

  Document document;
  ASSERT_TRUE(document.load(root / "with.xodr").has_value());
  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);
  ASSERT_TRUE(document.load(root / "without.xodr").has_value());
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty())
      << "the previous scene's actors survived a load";
}

TEST(ScenarioDocument, ResetClearsTheScenario) {
  Document document;
  place_an_actor(document, "Car1");
  ASSERT_TRUE(document.has_scenario());

  document.reset();
  EXPECT_FALSE(document.has_scenario());
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty());
}

// --- the ONE stack ----------------------------------------------------------

TEST(ScenarioDocument, ScenarioCommandsShareTheMapsUndoStack) {
  // ★ THE INVARIANT CLAUDE.md MANDATES, and the one that makes GW-6 step 1
  // true: there is exactly ONE stack. A scenario edit lands on it beside the
  // map edits, so the counts are cumulative rather than parallel.
  Document document;
  author_a_road(document);
  const int after_map = document.undo_stack()->count();
  ASSERT_GT(after_map, 0);

  place_an_actor(document, "Car1");
  EXPECT_EQ(document.undo_stack()->count(), after_map + 2)
      << "a scenario command did not land on the map's stack";
}

TEST(ScenarioDocument, UndoingPastAScenarioEditReachesTheMapEditBeforeIt) {
  // The consequence that matters. Interleaved history means undo walks BACK
  // THROUGH the scenario edits into the map ones — which is exactly what
  // "switching back to Map mode returns to it with the undo history intact"
  // has to mean when both documents share a stack.
  Document document;
  author_a_road(document);
  const std::size_t roads_after_authoring = document.network().road_count();
  ASSERT_GT(roads_after_authoring, 0U);

  place_an_actor(document, "Car1");
  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);

  document.undo_stack()->undo(); // the placement
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty());
  EXPECT_EQ(document.network().road_count(), roads_after_authoring) << "undoing a scenario "
                                                                       "command touched the map";

  document.undo_stack()->undo(); // the logic file
  document.undo_stack()->undo(); // and now the ROAD
  EXPECT_EQ(document.network().road_count(), roads_after_authoring - 1U)
      << "the map edit beneath the scenario edits was not reachable";
}

TEST(ScenarioDocument, PlacingAnActorIsOneUndoEntryAndOneRedoRestoresIt) {
  Document document;
  place_an_actor(document, "Car1");
  const int count = document.undo_stack()->count();

  document.undo_stack()->undo();
  EXPECT_TRUE(document.scenario().entities.scenario_objects.empty());
  document.undo_stack()->redo();
  ASSERT_EQ(document.scenario().entities.scenario_objects.size(), 1U);
  EXPECT_EQ(document.scenario().entities.scenario_objects[0].name, "Car1");
  EXPECT_EQ(document.undo_stack()->count(), count);
}

TEST(ScenarioDocument, UndoRedoLeavesTheScenarioByteIdentical) {
  // The kernel's contract, re-asserted at the EDITOR boundary — the bridge
  // could satisfy it in the kernel and still lose content by mis-sequencing
  // apply/revert here.
  Document document;
  place_an_actor(document, "Car1");
  ASSERT_TRUE(document
                  .push_scenario_command(
                      osc::edit::set_entity_init_speed(document.scenario(), "Car1", 13.89))
                  .has_value());

  const auto before = osc::write_xosc(document.scenario());
  ASSERT_TRUE(before.has_value());

  for (int i = 0; i < 10; ++i) {
    document.undo_stack()->undo();
    document.undo_stack()->undo();
    document.undo_stack()->redo();
    document.undo_stack()->redo();
  }

  const auto after = osc::write_xosc(document.scenario());
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, *before);
}

TEST(ScenarioDocument, AFailedScenarioCommandChangesNothingAndIsNotPushed) {
  Document document;
  place_an_actor(document, "Car1");
  const int count = document.undo_stack()->count();
  const auto before = osc::write_xosc(document.scenario());
  ASSERT_TRUE(before.has_value());

  // A duplicate @name — refused by the factory, and the refusal is a Command.
  const auto pushed = document.push_scenario_command(osc::edit::place_scenario_object(
      document.scenario(), osc::make_actor(osc::ActorKind::Car, "Car1"), lane_at(50.0)));
  EXPECT_FALSE(pushed.has_value());
  EXPECT_EQ(document.undo_stack()->count(), count) << "a refused command was pushed";
  EXPECT_EQ(*osc::write_xosc(document.scenario()), *before);
}

TEST(ScenarioDocument, ANullScenarioCommandIsRefusedRatherThanCrashing) {
  Document document;
  EXPECT_FALSE(document.push_scenario_command(nullptr).has_value());
}

TEST(ScenarioDocument, ScenarioChangedFiresOnEveryMutationAndOnLoad) {
  Document document;
  QSignalSpy spy(&document, &Document::scenario_changed);
  ASSERT_TRUE(spy.isValid());

  place_an_actor(document, "Car1");
  EXPECT_EQ(spy.count(), 2) << "one signal per pushed command";

  document.undo_stack()->undo();
  EXPECT_EQ(spy.count(), 3) << "an undo must republish the scenario";

  document.reset();
  EXPECT_EQ(spy.count(), 4) << "a reset must republish the (now empty) scenario";
}

TEST(ScenarioDocument, AMalformedScenarioWarnsAndCostsNoScene) {
  // The `.xodr` alone is the scene. A broken `.xosc` must never fail the load —
  // but it must SAY so, or the actors simply appear to be missing.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path scene = std::filesystem::path(dir.path().toStdString()) / "town.xodr";

  Document writer;
  author_a_road(writer);
  ASSERT_TRUE(writer.save(scene).has_value());
  {
    std::ofstream broken(scene.parent_path() / "town.xosc", std::ios::binary);
    broken << "<OpenSCENARIO><Entities>"; // truncated
  }

  Document reader;
  ASSERT_TRUE(reader.load(scene).has_value()) << "a broken scenario failed the whole load";
  EXPECT_GT(reader.network().road_count(), 0U) << "the scene was lost to a scenario problem";

  bool reported = false;
  for (const Diagnostic& finding : reader.diagnostics()) {
    reported = reported || finding.message.find("scenario") != std::string::npos;
  }
  EXPECT_TRUE(reported) << "a malformed scenario was dropped without a diagnostic";
}

} // namespace roadmaker::editor
