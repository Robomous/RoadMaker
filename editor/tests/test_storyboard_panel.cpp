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

// The Storyboard / condition editor (p8-s4, issue #248), driven headlessly
// through the same public methods the widgets' handlers call.
//
// THE PROPERTIES THAT MATTER: every gesture is ONE undo entry through the
// document's single stack, every add produces a document `write_xosc` accepts
// (a half-built story would make the scenario unsavable by an edit that
// appeared to succeed), and every `@phase` the panel offers comes from
// `osc::phase_names` rather than `Phase::name` — the trap #248 was filed with.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <variant>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/storyboard_panel.hpp"

namespace roadmaker::editor {
namespace {

/// A scenario with two actors and one signalized controller, pushed through the
/// same kernel commands the editor uses.
void author_scenario(Document& document) {
  osc::ScenarioObject ego;
  ego.name = "Ego";
  ego.entity_object = osc::Vehicle{};
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), ego, osc::WorldPosition{}))
                  .has_value());

  osc::ScenarioObject target;
  target.name = "Target";
  target.entity_object = osc::Vehicle{};
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::place_scenario_object(
                      document.scenario(), target, osc::WorldPosition{}))
                  .has_value());

  ASSERT_TRUE(
      document.push_scenario_command(osc::edit::set_logic_file(document.scenario(), "n.xodr"))
          .has_value());
}

RoadId make_road(Document& document, double x0, double y0, double x1, double y1, const char* odr) {
  EXPECT_TRUE(
      document
          .push_command(edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                          LaneProfile::two_lane_default(),
                                          odr))
          .has_value());
  return document.network().find_road(odr);
}

/// A signalized four-arm junction, decomposed into the scenario the way the
/// editor does it — through `sync_traffic_signals`, so the controllers under
/// test are the ones a real scene produces rather than hand-assembled ones.
void add_signalized_junction(Document& document) {
  const std::array<RoadEnd, 4> ends{
      RoadEnd{make_road(document, -80.0, 0.0, -20.0, 0.0, "1"), ContactPoint::End},
      RoadEnd{make_road(document, 80.0, 0.0, 20.0, 0.0, "2"), ContactPoint::End},
      RoadEnd{make_road(document, 0.0, -80.0, 0.0, -20.0, "3"), ContactPoint::End},
      RoadEnd{make_road(document, 0.0, 80.0, 0.0, 20.0, "4"), ContactPoint::End}};
  ASSERT_TRUE(document.push_command(edit::create_junction(document.network(), ends)).has_value());

  JunctionId junction;
  document.network().for_each_junction([&](JunctionId id, const Junction&) { junction = id; });
  ASSERT_TRUE(junction.is_valid());
  ASSERT_TRUE(document
                  .push_command(edit::signalize_junction(
                      document.network(), junction, {.tmpl = edit::SignalizeTemplate::TwoPhase}))
                  .has_value());
  ASSERT_TRUE(document
                  .push_scenario_command(osc::edit::sync_traffic_signals(
                      document.scenario(), document.network(), junction))
                  .has_value());
  ASSERT_FALSE(document.scenario().road_network.traffic_signal_controllers.empty());
}

std::string written(const Document& document) {
  const auto text = osc::write_xosc(document.scenario());
  EXPECT_TRUE(text.has_value()) << (text.has_value() ? "" : text.error().message);
  return text.value_or(std::string{});
}

} // namespace

TEST(StoryboardPanel, AddCreatesAWritableStoryAndIsOneUndoEntry) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  const int before = document.undo_stack()->count();
  ASSERT_TRUE(document.scenario().storyboard.stories.empty());

  panel.add_child();

  ASSERT_EQ(document.scenario().storyboard.stories.size(), 1U);
  EXPECT_EQ(document.undo_stack()->count(), before + 1) << "one gesture, one undo entry";
  EXPECT_EQ(panel.current_path().level, StoryboardLevel::Story);

  // ★ A COMPLETE SUBTREE, not a bare <Story>. Every level below has a schema
  // minimum of one, so an empty act would make the document unsavable by an
  // edit that appeared to succeed.
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
  EXPECT_FALSE(written(document).empty());
}

TEST(StoryboardPanel, AddWalksDownTheSchemaAndUndoRestoresTheBytes) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  // The stack is SHARED with the map's and with the setup above (Document has
  // exactly one), so "undo everything" would undo the actors too. The fixed
  // point being measured is the panel's own edits.
  const int setup = document.undo_stack()->index();
  const std::string empty = written(document);

  panel.add_child(); // story
  const std::string with_story = written(document);
  panel.add_child(); // act (the story is selected)
  panel.add_child(); // maneuver group
  panel.add_child(); // maneuver
  panel.add_child(); // event
  panel.add_child(); // action
  EXPECT_EQ(panel.current_path().level, StoryboardLevel::Action);
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());

  // An action has no child; the sixth click is a no-op, not a crash or a
  // seventh undo entry.
  const int count = document.undo_stack()->count();
  panel.add_child();
  EXPECT_EQ(document.undo_stack()->count(), count);

  // apply -> revert is byte-identical, all the way down.
  while (document.undo_stack()->index() > setup) {
    document.undo_stack()->undo();
  }
  EXPECT_EQ(written(document), empty);
  document.undo_stack()->redo();
  EXPECT_EQ(written(document), with_story);
}

TEST(StoryboardPanel, RemovingTheLastChildRemovesTheParentRatherThanLeavingItInvalid) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child(); // story, with a complete act/group/maneuver/event/action

  // Select the single action and remove it. Its event is the maneuver's only
  // one, so the EVENT goes too rather than leaving an <Event> with no <Action>
  // — which the schema forbids and `write_xosc` refuses.
  panel.select_path({.level = StoryboardLevel::Action});
  ASSERT_EQ(panel.current_path().level, StoryboardLevel::Action);
  panel.remove_selected();

  // ★ AND IT STOPS THERE. A <ManeuverGroup> may legally hold NO maneuver
  // (minOccurs 0), unlike every other level, so the cascade goes exactly as far
  // as the schema requires and no further — deleting one action must not
  // silently delete the story.
  ASSERT_EQ(document.scenario().storyboard.stories.size(), 1U);
  const osc::ManeuverGroup& group =
      document.scenario().storyboard.stories[0].acts[0].maneuver_groups[0];
  EXPECT_TRUE(group.maneuvers.empty());
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
  EXPECT_FALSE(written(document).empty());
}

TEST(StoryboardPanel, RemovingOneOfSeveralSiblingsLeavesTheRestValid) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child(); // story
  panel.select_path({.level = StoryboardLevel::Story});
  panel.add_child(); // a SECOND act

  ASSERT_EQ(document.scenario().storyboard.stories[0].acts.size(), 2U);
  panel.remove_selected();
  ASSERT_EQ(document.scenario().storyboard.stories[0].acts.size(), 1U);
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
}

TEST(StoryboardPanel, AddedSiblingsGetUniqueNamesRatherThanADuplicate) {
  // <Act> names are unique among siblings, and the writer REFUSES a duplicate —
  // so clicking Add twice must not produce a document that cannot be saved.
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child(); // story
  panel.select_path({.level = StoryboardLevel::Story});
  panel.add_child();
  panel.select_path({.level = StoryboardLevel::Story});
  panel.add_child();

  const std::vector<osc::Act>& acts = document.scenario().storyboard.stories[0].acts;
  ASSERT_EQ(acts.size(), 3U);
  EXPECT_NE(acts[0].name, acts[1].name);
  EXPECT_NE(acts[1].name, acts[2].name);
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
}

TEST(StoryboardPanel, EveryPhaseChoiceIsAPhaseNameTheFileActuallyCarries) {
  // ★ THE HEADLINE OF #248, asserted against the DOCUMENT rather than against
  // `osc::phase_names` — comparing the panel's list to the function it calls
  // would be a tautology. What matters is that every label the phase combo
  // offers appears as a `<Phase @name>` in the file the user then saves; a
  // combo populated from `Phase::name` can offer one that does not, and esmini
  // was measured to accept the dangling result in silence.
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  add_signalized_junction(document);
  StoryboardPanel panel(document, selection);

  const osc::TrafficSignalController& controller =
      document.scenario().road_network.traffic_signal_controllers[0];
  const std::vector<QString> choices = panel.phase_choices(QString::fromStdString(controller.name));
  ASSERT_EQ(choices.size(), controller.phases.size());

  const std::string text = written(document);
  for (const QString& choice : choices) {
    const std::string needle = "<Phase name=\"" + choice.toStdString() + "\"";
    EXPECT_NE(text.find(needle), std::string::npos) << needle << " is offered but never written";
  }

  EXPECT_TRUE(panel.phase_choices(QStringLiteral("no-such-controller")).empty());
}

TEST(StoryboardPanel, AControllerPhaseActionAuthoredThroughThePanelValidates) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  add_signalized_junction(document);
  StoryboardPanel panel(document, selection);

  const QString controller =
      QString::fromStdString(document.scenario().road_network.traffic_signal_controllers[0].name);

  panel.add_child(); // a whole story
  panel.select_path({.level = StoryboardLevel::Action});
  panel.set_action_kind(ActionKind::TrafficSignalPhase);
  EXPECT_EQ(panel.selected_action_kind(), ActionKind::TrafficSignalPhase);

  // Authored through the same synthesis the writer will use — which is the
  // only way this reference resolves.
  const std::vector<QString> choices = panel.phase_choices(controller);
  ASSERT_FALSE(choices.empty());
  panel.set_action_controller(controller, choices.front());

  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
  EXPECT_NE(written(document).find("<TrafficSignalControllerAction"), std::string::npos);

  // And a phase that is NOT in that list is exactly what validate_scenario
  // refuses — the check esmini was measured not to make.
  panel.set_action_controller(controller, QStringLiteral("not_a_phase"));
  EXPECT_FALSE(osc::validate_scenario(document.scenario()).empty());
}

TEST(StoryboardPanel, RetypingAnActionKeepsItsNameAndReplacesItsArm) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child();
  panel.select_path({.level = StoryboardLevel::Action});
  EXPECT_EQ(panel.selected_action_kind(), ActionKind::LaneChange);

  panel.set_selected_name(QStringLiteral("my_action"));
  panel.select_path({.level = StoryboardLevel::Action});
  ASSERT_EQ(panel.selected_name(), QStringLiteral("my_action"));

  panel.set_action_kind(ActionKind::Speed);
  EXPECT_EQ(panel.selected_action_kind(), ActionKind::Speed);
  EXPECT_EQ(panel.selected_name(), QStringLiteral("my_action"))
      << "the name is the user's label, not part of what the action does";
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());

  // Retyping to the SAME kind is a no-op, not a second undo entry.
  const int count = document.undo_stack()->count();
  panel.set_action_kind(ActionKind::Speed);
  EXPECT_EQ(document.undo_stack()->count(), count);
}

TEST(StoryboardPanel, ATriggerIsRemovedRatherThanEmptiedWhenTheConditionIsCleared) {
  // An EMPTY <StartTrigger> is an element the file did not have; "no trigger"
  // is its own legal state ("the event starts when the act enters
  // runningState"), so clearing must remove the element.
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child();
  panel.select_path({.level = StoryboardLevel::Event});
  EXPECT_EQ(panel.selected_condition_kind(), ConditionKind::None);
  EXPECT_EQ(written(document).find("<StartTrigger"), std::string::npos);

  panel.set_condition_kind(ConditionKind::RelativeDistance);
  EXPECT_EQ(panel.selected_condition_kind(), ConditionKind::RelativeDistance);
  EXPECT_NE(written(document).find("<RelativeDistanceCondition"), std::string::npos);
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());

  panel.set_condition_kind(ConditionKind::None);
  EXPECT_EQ(written(document).find("<StartTrigger"), std::string::npos);
  EXPECT_TRUE(osc::validate_scenario(document.scenario()).empty());
}

TEST(StoryboardPanel, ARelativeDistanceConditionNeverMeasuresAnEntityAgainstItself) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child();
  panel.select_path({.level = StoryboardLevel::Event});
  panel.set_condition_kind(ConditionKind::RelativeDistance);
  panel.set_condition_entity(QStringLiteral("Target"));

  const osc::Event& event =
      document.scenario().storyboard.stories[0].acts[0].maneuver_groups[0].maneuvers[0].events[0];
  ASSERT_TRUE(event.start_trigger.has_value());
  const osc::Condition& condition = event.start_trigger->condition_groups[0].conditions[0];
  ASSERT_TRUE(condition.by_entity.has_value());
  ASSERT_EQ(condition.by_entity->triggering_entities.entity_refs.size(), 1U);
  EXPECT_EQ(condition.by_entity->triggering_entities.entity_refs[0].entity_ref, "Target");

  const auto* distance =
      std::get_if<osc::RelativeDistanceCondition>(&condition.by_entity->entity_condition);
  ASSERT_NE(distance, nullptr);
  EXPECT_NE(distance->entity_ref, "Target")
      << "measuring a car's distance to itself is always zero";
}

TEST(StoryboardPanel, AStalePathCollapsesToTheNearestLiveAncestor) {
  // A path that outlived its node must never index into a story the user did
  // not choose — the p6-s7 lesson, applied to a six-deep path.
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child();
  panel.select_path({.level = StoryboardLevel::Action, .story = 0, .act = 9, .action = 7});
  EXPECT_EQ(panel.current_path().level, StoryboardLevel::Story)
      << "act 9 does not exist, so the path stops at the story";

  panel.select_path({.level = StoryboardLevel::Story, .story = 99});
  EXPECT_EQ(panel.current_path().level, StoryboardLevel::Root);
}

TEST(StoryboardPanel, TheTreeShowsEveryLevelOfTheStory) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  EXPECT_EQ(panel.row_count(), 0);
  panel.add_child();
  // Story, act, group, maneuver, event, action — six rows for the default tree.
  EXPECT_EQ(panel.row_count(), 6);
}

TEST(StoryboardPanel, EventPriorityIsWrittenAndDefaultsToASpellingRevisionOnePointTwoAdmits) {
  Document document;
  SelectionModel selection(document);
  author_scenario(document);
  StoryboardPanel panel(document, selection);

  panel.add_child();
  EXPECT_NE(written(document).find(R"(priority="overwrite")"), std::string::npos);

  panel.select_path({.level = StoryboardLevel::Event});
  panel.set_event_priority(QStringLiteral("parallel"));
  EXPECT_NE(written(document).find(R"(priority="parallel")"), std::string::npos);
}

} // namespace roadmaker::editor
