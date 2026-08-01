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

// The Storyboard / condition editor (p8-s4, issue #248, GW-6): a tree of
// Story ▸ Act ▸ ManeuverGroup ▸ Maneuver ▸ Event ▸ Action beside a form for
// whichever node is selected, hosted as a page of the 2D Editor pane next to
// the Signal Phase Editor (which is the precedent this follows).
//
// EVERY EDIT IS ONE KERNEL COMMAND THROUGH Document, exactly as PhasePanel's
// are. The command is `osc::edit::set_story`, which replaces a whole `<Story>`:
// the panel builds the modified value and pushes it, so one gesture is one undo
// entry and the byte-identity contract holds without thirty per-node factories
// (roadmaker/osc/edit.hpp says why at length).
//
// ★ EVERY @phase THE PANEL AUTHORS COMES FROM `osc::phase_names()`, never from
// `Phase::name`. That is the trap #248 was filed with: `Phase::name` may legally
// be empty, and the writer synthesizes names into the OUTPUT only — so a phase
// combo populated from the model offers labels that match nothing in the file
// the user then saves. `phase_choices()` is the single place this resolves.
//
// The interactive entry points are PUBLIC METHODS the offscreen tests drive
// directly; the widgets' signal handlers call the same methods, so the tests
// exercise the code the mouse does.

#include "roadmaker/osc/scenario.hpp"

#include <QString>
#include <QWidget>
#include <cstddef>
#include <optional>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// Which storyboard level a tree row is.
///
/// `Trigger` is a row and not a level of the model: an event's `<StartTrigger>`
/// is shown as a child row so a condition can be selected and edited, but it is
/// addressed through its owning event.
enum class StoryboardLevel {
  Root,
  Story,
  Act,
  ManeuverGroup,
  Maneuver,
  Event,
  Action,
};

/// The path from the storyboard root to one node. Indices past the node's own
/// level are meaningless and left at zero.
struct StoryboardPath {
  StoryboardLevel level = StoryboardLevel::Root;
  std::size_t story = 0;
  std::size_t act = 0;
  std::size_t maneuver_group = 0;
  std::size_t maneuver = 0;
  std::size_t event = 0;
  std::size_t action = 0;
};

/// What an `<Action>` does — the palette the panel can author.
enum class ActionKind {
  LaneChange,         ///< <LateralAction><LaneChangeAction>, the cut-in
  Speed,              ///< <LongitudinalAction><SpeedAction>
  TrafficSignalState, ///< <GlobalAction>…<TrafficSignalStateAction>
  TrafficSignalPhase, ///< <GlobalAction>…<TrafficSignalControllerAction>
  Preserved,          ///< an arm read from a file that this version does not model
};

/// What a `<Condition>` tests — the palette the panel can author.
enum class ConditionKind {
  None, ///< no <StartTrigger> at all, which is legal and means "start with the act"
  SimulationTime,
  RelativeDistance,
  Speed,
  TrafficSignalState,
  TrafficSignalPhase,
  StoryboardElementState,
  Preserved, ///< an arm read from a file that this version does not model
};

class StoryboardPanel : public QWidget {
  Q_OBJECT

public:
  StoryboardPanel(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  // --- view state (NEVER touches undo/the document) --------------------------

  /// The node the form is editing.
  [[nodiscard]] StoryboardPath current_path() const { return path_; }

  /// Selects `path` in the tree. A path the storyboard does not have collapses
  /// to the nearest ancestor it does — never to an out-of-range index, which is
  /// what would let a stale selection write into a story the user never chose.
  void select_path(const StoryboardPath& path);

  /// Row count of the tree, for tests and for the empty-state label.
  [[nodiscard]] int row_count() const;

  /// The phase names selectable for `controller_odr_id` — `osc::phase_names`,
  /// which is the SYNTHESIS the writer performs, not `Phase::name`.
  [[nodiscard]] std::vector<QString> phase_choices(const QString& controller_odr_id) const;

  // --- edits (each pushes exactly ONE command; a no-op is skipped) -----------

  /// Adds the schema-appropriate child of the selected node: a story at the
  /// root, an act under a story, and so on down to an action under an event.
  /// A selected action has no child and this is a no-op.
  ///
  /// ★ ADDS A COMPLETE SUBTREE, not a bare element. `<Act>` requires a
  /// `<ManeuverGroup>`, which requires a `<Maneuver>`, which requires an
  /// `<Event>`, which requires an `<Action>` — so adding an empty act would
  /// produce a document `write_xosc` refuses, i.e. an edit that appears to
  /// succeed and then makes the scenario unsavable.
  void add_child();

  /// Removes the selected node. Removing the last child of a parent that
  /// requires one removes the PARENT too, for the reason above; removing the
  /// last story leaves the storyboard empty, which is legal.
  void remove_selected();

  /// Renames the selected node, and nothing else — a rename that left a
  /// `StoryboardElementStateCondition` pointing at the old name is the caller's
  /// to fix, and the Diagnostics dock reports it.
  void set_selected_name(const QString& name);

  /// `<Event @priority>`. Only meaningful for an event; a no-op elsewhere.
  void set_event_priority(const QString& priority);

  /// The selected maneuver group's `<Actors>` — the entities its maneuvers act
  /// on. May legally be empty: an infrastructure-only group has no actor.
  void set_group_actors(const std::vector<QString>& entity_names);

  /// Retypes the selected `<Action>`, replacing its arm with a default-built
  /// one of `kind`. Retyping DISCARDS the old arm's fields, which is what
  /// changing what an action does means.
  void set_action_kind(ActionKind kind);

  /// The selected action's parameters. Which of these apply depends on its
  /// kind; one that does not apply is ignored rather than refused, so a form
  /// that pushes all of them on every change stays correct.
  void set_action_entity(const QString& entity_name);
  void set_action_value(double value);
  void set_action_signal_id(const QString& odr_id);
  void set_action_state(const QString& state);
  void set_action_controller(const QString& controller_odr_id, const QString& phase);

  /// Retypes the selected event's `<StartTrigger>`; `ConditionKind::None`
  /// removes the trigger entirely, which is the legal "start with the act"
  /// state and NOT an empty `<StartTrigger>` element.
  void set_condition_kind(ConditionKind kind);
  void set_condition_entity(const QString& entity_name);
  void set_condition_value(double value);
  void set_condition_rule(const QString& rule);
  void set_condition_state(const QString& state);
  void set_condition_controller(const QString& controller_odr_id, const QString& phase);

  // --- pull getters (tests, and the form's own repopulation) -----------------

  [[nodiscard]] ActionKind selected_action_kind() const;
  [[nodiscard]] ConditionKind selected_condition_kind() const;
  [[nodiscard]] QString selected_name() const;

signals:
  /// The tree selection or the document changed — the seam a host re-reads.
  void storyboard_view_changed();

private:
  /// Rebuilds the tree from the scenario, restoring the selection BY PATH.
  /// Reset-based like the Library tree: an index-based restore lands on a
  /// different node the moment a sibling is inserted above it.
  void rebuild();

  /// Shows only the fields the selected node has, and fills them.
  void refresh_form();

  /// The story the current path names, or nullptr.
  [[nodiscard]] const osc::Story* current_story() const;

  /// The selected node's owning story, MUTATED IN A COPY and pushed as one
  /// `set_story`. Returns false when the path names nothing.
  [[nodiscard]] bool commit(const osc::Story& story);

  /// `commit` for a FIELD edit, where the panel has nothing left to do either
  /// way: `Document::push_scenario_command` already appends a refusal to the
  /// diagnostics, and a refused command left the document untouched — so the
  /// form still shows exactly what the document holds. The structural edits do
  /// read the result, because they must not move the tree selection to a node
  /// that was never created.
  void commit_field(const osc::Story& story) { (void)commit(story); }

  /// The `<Event>` the current path names inside `story`, or nullptr. Used on a
  /// COPY of the story the edit is about to push.
  [[nodiscard]] osc::Event* event_in(osc::Story& story) const;
  [[nodiscard]] osc::Action* action_in(osc::Story& story) const;

  /// Clamps `path` to what the storyboard actually holds.
  [[nodiscard]] StoryboardPath clamped(const StoryboardPath& path) const;

  Document& document_;
  SelectionModel& selection_;

  QTreeWidget* tree_;
  QPushButton* add_button_;
  QPushButton* remove_button_;

  QLineEdit* name_edit_;
  QComboBox* priority_combo_;
  QComboBox* action_kind_combo_;
  QComboBox* condition_kind_combo_;
  QComboBox* entity_combo_;
  QComboBox* rule_combo_;
  QDoubleSpinBox* value_spin_;
  QComboBox* controller_combo_;
  QComboBox* phase_combo_;
  QLineEdit* state_edit_;
  QLabel* empty_label_;

  StoryboardPath path_;

  /// True while `rebuild()`/`refresh_form()` are writing the widgets, so their
  /// change signals do not push a command for a value the panel just displayed.
  bool populating_ = false;
};

} // namespace roadmaker::editor
