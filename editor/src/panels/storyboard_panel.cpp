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

#include "panels/storyboard_panel.hpp"

#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/writer.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <string>
#include <utility>
#include <variant>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

/// The path a tree row carries, round-tripped through QVariant.
///
/// Six indices packed into one QVariantList rather than a registered metatype:
/// a metatype would be a second declaration to keep in step with the struct,
/// and the tree is the only consumer.
QVariant to_variant(const StoryboardPath& path) {
  return QVariantList{static_cast<int>(path.level),
                      static_cast<qulonglong>(path.story),
                      static_cast<qulonglong>(path.act),
                      static_cast<qulonglong>(path.maneuver_group),
                      static_cast<qulonglong>(path.maneuver),
                      static_cast<qulonglong>(path.event),
                      static_cast<qulonglong>(path.action)};
}

StoryboardPath from_variant(const QVariant& value) {
  const QVariantList parts = value.toList();
  if (parts.size() != 7) {
    return {};
  }
  StoryboardPath path;
  path.level = static_cast<StoryboardLevel>(parts[0].toInt());
  path.story = static_cast<std::size_t>(parts[1].toULongLong());
  path.act = static_cast<std::size_t>(parts[2].toULongLong());
  path.maneuver_group = static_cast<std::size_t>(parts[3].toULongLong());
  path.maneuver = static_cast<std::size_t>(parts[4].toULongLong());
  path.event = static_cast<std::size_t>(parts[5].toULongLong());
  path.action = static_cast<std::size_t>(parts[6].toULongLong());
  return path;
}

QString q(const std::string& text) {
  return QString::fromStdString(text);
}

/// What a default-built action or condition needs in order to be COMPLETE.
///
/// ★ EVERY DEFAULT MUST BE WRITABLE. `validate_scenario` refuses an empty
/// `@entityRef`, `@trafficSignalId`, `@trafficSignalControllerRef` or
/// `@storyboardElementRef` — so a default-constructed arm would make the
/// document unsavable the moment the user picked its type from the combo, which
/// is an edit that appears to succeed and then breaks Save. These are the live
/// references the panel seeds with instead.
struct Seed {
  QString first_entity;
  /// A DIFFERENT entity where one exists — measuring a car's distance to
  /// itself is always zero.
  QString other_entity;
  QString first_signal_id;
  QString first_controller;
  /// The first controller's first phase name AS THE WRITER WILL SPELL IT
  /// (`osc::phase_names`), never `Phase::name`. See this file's header.
  QString first_phase;
  QString element_ref;
  QString signal_state;
};

Seed seed_from(const osc::Scenario& scenario, const QString& element_ref) {
  Seed seed;
  seed.element_ref = element_ref;
  for (const osc::ScenarioObject& object : scenario.entities.scenario_objects) {
    if (seed.first_entity.isEmpty()) {
      seed.first_entity = q(object.name);
    } else if (seed.other_entity.isEmpty()) {
      seed.other_entity = q(object.name);
    }
  }
  if (seed.other_entity.isEmpty()) {
    seed.other_entity = seed.first_entity;
  }

  const auto& controllers = scenario.road_network.traffic_signal_controllers;
  if (!controllers.empty()) {
    seed.first_controller = q(controllers.front().name);
    const std::vector<std::string> names = osc::phase_names(controllers.front());
    if (!names.empty()) {
      seed.first_phase = q(names.front());
    }
    for (const osc::Phase& phase : controllers.front().phases) {
      if (!phase.signal_states.empty()) {
        seed.first_signal_id = q(phase.signal_states.front().traffic_signal_id);
        seed.signal_state = q(phase.signal_states.front().state);
        break;
      }
    }
  }
  return seed;
}

/// A default `<Action>` of each kind. Every one is complete enough to WRITE:
/// a half-built action would make the document unsavable by an edit that
/// appeared to succeed.
osc::Action default_action(ActionKind kind, const Seed& seed) {
  const QString& first_entity = seed.first_entity;
  osc::Action action;
  switch (kind) {
  case ActionKind::LaneChange: {
    osc::LaneChangeAction change;
    change.dynamics.dynamics_shape = std::string{osc::kDefaultLaneChangeShape};
    change.dynamics.value = osc::kDefaultLaneChangeDuration;
    change.target = osc::RelativeTargetLane{
        .entity_ref = first_entity.toStdString(), .value = 1, .preserved = {}};
    osc::LateralAction lateral;
    lateral.lane_change = std::move(change);
    osc::PrivateAction entry;
    entry.lateral = std::move(lateral);
    action.name = "lane_change";
    action.action = std::move(entry);
    break;
  }
  case ActionKind::Speed: {
    osc::SpeedAction speed;
    speed.absolute_target = osc::AbsoluteTargetSpeed{};
    osc::LongitudinalAction longitudinal;
    longitudinal.speed = std::move(speed);
    osc::PrivateAction entry;
    entry.longitudinal = std::move(longitudinal);
    action.name = "set_speed";
    action.action = std::move(entry);
    break;
  }
  case ActionKind::TrafficSignalState: {
    osc::TrafficSignalAction signal;
    signal.action = osc::TrafficSignalStateAction{.name = seed.first_signal_id.toStdString(),
                                                  .state = seed.signal_state.toStdString(),
                                                  .preserved = {}};
    osc::InfrastructureAction infrastructure;
    infrastructure.traffic_signal = std::move(signal);
    osc::GlobalAction global;
    global.infrastructure = std::move(infrastructure);
    action.name = "signal_state";
    action.action = std::move(global);
    break;
  }
  case ActionKind::TrafficSignalPhase: {
    osc::TrafficSignalAction signal;
    signal.action = osc::TrafficSignalControllerAction{.traffic_signal_controller_ref =
                                                           seed.first_controller.toStdString(),
                                                       .phase = seed.first_phase.toStdString(),
                                                       .preserved = {}};
    osc::InfrastructureAction infrastructure;
    infrastructure.traffic_signal = std::move(signal);
    osc::GlobalAction global;
    global.infrastructure = std::move(infrastructure);
    action.name = "signal_phase";
    action.action = std::move(global);
    break;
  }
  case ActionKind::Preserved:
    action.name = "action";
    break;
  }
  return action;
}

/// A default `<Condition>` of each kind, ditto.
osc::Condition default_condition(ConditionKind kind, const Seed& seed) {
  osc::Condition condition;
  condition.name = "condition";
  switch (kind) {
  case ConditionKind::None:
  case ConditionKind::Preserved:
    break;
  case ConditionKind::SimulationTime:
    condition.simulation_time = osc::SimulationTimeCondition{};
    break;
  case ConditionKind::RelativeDistance: {
    osc::ByEntityCondition by_entity;
    by_entity.triggering_entities.entity_refs.push_back(
        osc::EntityRef{.entity_ref = seed.first_entity.toStdString(), .preserved = {}});
    osc::RelativeDistanceCondition distance;
    distance.entity_ref = seed.other_entity.toStdString();
    by_entity.entity_condition = std::move(distance);
    condition.by_entity = std::move(by_entity);
    break;
  }
  case ConditionKind::Speed: {
    osc::ByEntityCondition by_entity;
    by_entity.triggering_entities.entity_refs.push_back(
        osc::EntityRef{.entity_ref = seed.first_entity.toStdString(), .preserved = {}});
    by_entity.entity_condition = osc::SpeedCondition{};
    condition.by_entity = std::move(by_entity);
    break;
  }
  case ConditionKind::TrafficSignalState:
    condition.traffic_signal =
        osc::TrafficSignalCondition{.name = seed.first_signal_id.toStdString(),
                                    .state = seed.signal_state.toStdString(),
                                    .preserved = {}};
    break;
  case ConditionKind::TrafficSignalPhase:
    condition.traffic_signal_controller = osc::TrafficSignalControllerCondition{
        .traffic_signal_controller_ref = seed.first_controller.toStdString(),
        .phase = seed.first_phase.toStdString(),
        .preserved = {}};
    break;
  case ConditionKind::StoryboardElementState:
    condition.storyboard_element_state = osc::StoryboardElementStateCondition{
        .storyboard_element_ref = seed.element_ref.toStdString(),
        .state = "completeState",
        .storyboard_element_type = "event",
        .preserved = {}};
    break;
  }
  return condition;
}

/// The selected event's first condition, or nullptr — the panel edits exactly
/// one condition per trigger, which is what "a condition editor" means at this
/// sprint's scope; a foreign trigger with several keeps all of them and the
/// panel edits the first.
osc::Condition* first_condition(osc::Event& event) {
  if (!event.start_trigger.has_value() || event.start_trigger->condition_groups.empty() ||
      event.start_trigger->condition_groups[0].conditions.empty()) {
    return nullptr;
  }
  return &event.start_trigger->condition_groups[0].conditions[0];
}

const osc::Condition* first_condition(const osc::Event& event) {
  return first_condition(const_cast<osc::Event&>(event)); // NOLINT: const-correct wrapper
}

/// A complete `<Event>` — one that carries the action every event requires.
osc::Event default_event(const Seed& seed) {
  osc::Event event;
  event.name = "event";
  event.actions.push_back(default_action(ActionKind::LaneChange, seed));
  return event;
}

osc::StoryManeuver default_maneuver(const Seed& seed) {
  osc::StoryManeuver maneuver;
  maneuver.name = "maneuver";
  maneuver.events.push_back(default_event(seed));
  return maneuver;
}

osc::ManeuverGroup default_group(const Seed& seed) {
  osc::ManeuverGroup group;
  group.name = "group";
  if (!seed.first_entity.isEmpty()) {
    group.actors.push_back(
        osc::EntityRef{.entity_ref = seed.first_entity.toStdString(), .preserved = {}});
  }
  group.maneuvers.push_back(default_maneuver(seed));
  return group;
}

osc::Act default_act(const Seed& seed) {
  osc::Act act;
  act.name = "act";
  act.maneuver_groups.push_back(default_group(seed));
  return act;
}

/// Makes `name` unique among `taken` by appending the first free `_1`, `_2`, …
/// The writer's own de-duplication rule, applied where the user can still see
/// the result — `validate_scenario` would otherwise refuse the second act
/// someone added by clicking Add twice.
std::string unique_name(std::string name, const std::vector<std::string>& taken) {
  const std::string base = name;
  for (unsigned suffix = 1; std::find(taken.begin(), taken.end(), name) != taken.end(); ++suffix) {
    name = base + "_" + std::to_string(suffix);
  }
  return name;
}

} // namespace

StoryboardPanel::StoryboardPanel(Document& document, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection), tree_(new QTreeWidget(this)),
      add_button_(new QPushButton(tr("Add"), this)),
      remove_button_(new QPushButton(tr("Remove"), this)), name_edit_(new QLineEdit(this)),
      priority_combo_(new QComboBox(this)), action_kind_combo_(new QComboBox(this)),
      condition_kind_combo_(new QComboBox(this)), entity_combo_(new QComboBox(this)),
      rule_combo_(new QComboBox(this)), value_spin_(new QDoubleSpinBox(this)),
      controller_combo_(new QComboBox(this)), phase_combo_(new QComboBox(this)),
      state_edit_(new QLineEdit(this)),
      empty_label_(new QLabel(tr("No story yet — Add creates one."), this)) {
  setObjectName(QStringLiteral("storyboard_panel"));
  tree_->setObjectName(QStringLiteral("storyboard_tree"));
  tree_->setHeaderLabels({tr("Storyboard")});
  tree_->setColumnCount(1);

  add_button_->setObjectName(QStringLiteral("storyboard_add"));
  remove_button_->setObjectName(QStringLiteral("storyboard_remove"));

  // The literal spellings are the SCHEMA's, not display labels: they are
  // written into the file, so translating them would corrupt it.
  priority_combo_->addItems({QStringLiteral("overwrite"),
                             QStringLiteral("override"),
                             QStringLiteral("parallel"),
                             QStringLiteral("skip")});
  rule_combo_->addItems(
      {QStringLiteral("lessThan"), QStringLiteral("greaterThan"), QStringLiteral("equalTo")});

  action_kind_combo_->addItem(tr("Lane change"), static_cast<int>(ActionKind::LaneChange));
  action_kind_combo_->addItem(tr("Speed"), static_cast<int>(ActionKind::Speed));
  action_kind_combo_->addItem(tr("Traffic signal state"),
                              static_cast<int>(ActionKind::TrafficSignalState));
  action_kind_combo_->addItem(tr("Traffic signal phase"),
                              static_cast<int>(ActionKind::TrafficSignalPhase));

  condition_kind_combo_->addItem(tr("None (start with the act)"),
                                 static_cast<int>(ConditionKind::None));
  condition_kind_combo_->addItem(tr("Simulation time"),
                                 static_cast<int>(ConditionKind::SimulationTime));
  condition_kind_combo_->addItem(tr("Relative distance"),
                                 static_cast<int>(ConditionKind::RelativeDistance));
  condition_kind_combo_->addItem(tr("Speed"), static_cast<int>(ConditionKind::Speed));
  condition_kind_combo_->addItem(tr("Traffic signal state"),
                                 static_cast<int>(ConditionKind::TrafficSignalState));
  condition_kind_combo_->addItem(tr("Traffic signal phase"),
                                 static_cast<int>(ConditionKind::TrafficSignalPhase));
  condition_kind_combo_->addItem(tr("Storyboard element state"),
                                 static_cast<int>(ConditionKind::StoryboardElementState));

  value_spin_->setRange(-10000.0, 10000.0);
  value_spin_->setDecimals(3);

  auto* buttons = new QHBoxLayout;
  buttons->addWidget(add_button_);
  buttons->addWidget(remove_button_);
  buttons->addStretch();

  auto* form = new QFormLayout;
  form->addRow(tr("Name"), name_edit_);
  form->addRow(tr("Priority"), priority_combo_);
  form->addRow(tr("Action"), action_kind_combo_);
  form->addRow(tr("Start when"), condition_kind_combo_);
  form->addRow(tr("Entity"), entity_combo_);
  form->addRow(tr("Rule"), rule_combo_);
  form->addRow(tr("Value"), value_spin_);
  form->addRow(tr("Controller"), controller_combo_);
  form->addRow(tr("Phase"), phase_combo_);
  form->addRow(tr("State"), state_edit_);

  auto* right = new QVBoxLayout;
  right->addWidget(empty_label_);
  right->addLayout(form);
  right->addStretch();

  auto* columns = new QHBoxLayout;
  columns->addWidget(tree_, 1);
  columns->addLayout(right, 1);

  auto* root = new QVBoxLayout(this);
  root->addLayout(buttons);
  root->addLayout(columns);

  connect(add_button_, &QPushButton::clicked, this, &StoryboardPanel::add_child);
  connect(remove_button_, &QPushButton::clicked, this, &StoryboardPanel::remove_selected);
  connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (populating_) {
      return;
    }
    const QList<QTreeWidgetItem*> selected = tree_->selectedItems();
    path_ = selected.isEmpty() ? StoryboardPath{}
                               : from_variant(selected.front()->data(0, Qt::UserRole));
    refresh_form();
    emit storyboard_view_changed();
  });

  // Every widget handler calls the same public method the tests drive, so the
  // tests exercise the code the mouse does.
  connect(name_edit_, &QLineEdit::editingFinished, this, [this] {
    if (!populating_) {
      set_selected_name(name_edit_->text());
    }
  });
  connect(priority_combo_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
    if (!populating_) {
      set_event_priority(value);
    }
  });
  connect(action_kind_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (!populating_ && index >= 0) {
      set_action_kind(static_cast<ActionKind>(action_kind_combo_->itemData(index).toInt()));
    }
  });
  connect(condition_kind_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (!populating_ && index >= 0) {
      set_condition_kind(
          static_cast<ConditionKind>(condition_kind_combo_->itemData(index).toInt()));
    }
  });
  connect(entity_combo_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
    if (populating_) {
      return;
    }
    // An entity applies to an action OR to a condition depending on which the
    // form is showing; both setters ignore what does not apply.
    if (path_.level == StoryboardLevel::Action) {
      set_action_entity(value);
    } else {
      set_condition_entity(value);
    }
  });
  connect(rule_combo_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
    if (!populating_) {
      set_condition_rule(value);
    }
  });
  connect(value_spin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
    if (populating_) {
      return;
    }
    if (path_.level == StoryboardLevel::Action) {
      set_action_value(value);
    } else {
      set_condition_value(value);
    }
  });
  connect(controller_combo_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
    if (populating_) {
      return;
    }
    // Repopulating the phase list is the whole point: the phases a controller
    // offers are ITS OWN synthesized names, and a stale list from the previous
    // controller is exactly the dangling @phase #248 is about.
    const std::vector<QString> phases = phase_choices(value);
    {
      const QSignalBlocker block(phase_combo_);
      phase_combo_->clear();
      for (const QString& phase : phases) {
        phase_combo_->addItem(phase);
      }
    }
    const QString phase = phases.empty() ? QString() : phases.front();
    if (path_.level == StoryboardLevel::Action) {
      set_action_controller(value, phase);
    } else {
      set_condition_controller(value, phase);
    }
  });
  connect(phase_combo_, &QComboBox::currentTextChanged, this, [this](const QString& value) {
    if (populating_) {
      return;
    }
    if (path_.level == StoryboardLevel::Action) {
      set_action_controller(controller_combo_->currentText(), value);
    } else {
      set_condition_controller(controller_combo_->currentText(), value);
    }
  });
  connect(state_edit_, &QLineEdit::editingFinished, this, [this] {
    if (populating_) {
      return;
    }
    if (path_.level == StoryboardLevel::Action) {
      set_action_state(state_edit_->text());
    } else {
      set_condition_state(state_edit_->text());
    }
  });

  connect(&document_, &Document::scenario_changed, this, &StoryboardPanel::rebuild);
  connect(&selection_, &SelectionModel::selection_changed, this, [this] {
    // The selection does not drive the tree — a storyboard is a document-level
    // structure, not a per-entity one. It only refreshes the entity choices,
    // which a newly placed actor changes.
    refresh_form();
  });

  rebuild();
}

// --- the tree -----------------------------------------------------------------

int StoryboardPanel::row_count() const {
  int count = 0;
  for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
    ++count;
  }
  return count;
}

void StoryboardPanel::rebuild() {
  populating_ = true;
  tree_->clear();

  const osc::Storyboard& storyboard = document_.scenario().storyboard;
  for (std::size_t story_index = 0; story_index < storyboard.stories.size(); ++story_index) {
    const osc::Story& story = storyboard.stories[story_index];
    auto* story_item = new QTreeWidgetItem(tree_, {q(story.name)});
    story_item->setData(
        0, Qt::UserRole, to_variant({.level = StoryboardLevel::Story, .story = story_index}));

    for (std::size_t act_index = 0; act_index < story.acts.size(); ++act_index) {
      const osc::Act& act = story.acts[act_index];
      auto* act_item = new QTreeWidgetItem(story_item, {q(act.name)});
      act_item->setData(
          0,
          Qt::UserRole,
          to_variant({.level = StoryboardLevel::Act, .story = story_index, .act = act_index}));

      for (std::size_t group_index = 0; group_index < act.maneuver_groups.size(); ++group_index) {
        const osc::ManeuverGroup& group = act.maneuver_groups[group_index];
        auto* group_item = new QTreeWidgetItem(act_item, {q(group.name)});
        group_item->setData(0,
                            Qt::UserRole,
                            to_variant({.level = StoryboardLevel::ManeuverGroup,
                                        .story = story_index,
                                        .act = act_index,
                                        .maneuver_group = group_index}));

        for (std::size_t maneuver_index = 0; maneuver_index < group.maneuvers.size();
             ++maneuver_index) {
          const osc::StoryManeuver& maneuver = group.maneuvers[maneuver_index];
          auto* maneuver_item = new QTreeWidgetItem(group_item, {q(maneuver.name)});
          maneuver_item->setData(0,
                                 Qt::UserRole,
                                 to_variant({.level = StoryboardLevel::Maneuver,
                                             .story = story_index,
                                             .act = act_index,
                                             .maneuver_group = group_index,
                                             .maneuver = maneuver_index}));

          for (std::size_t event_index = 0; event_index < maneuver.events.size(); ++event_index) {
            const osc::Event& event = maneuver.events[event_index];
            auto* event_item = new QTreeWidgetItem(maneuver_item, {q(event.name)});
            event_item->setData(0,
                                Qt::UserRole,
                                to_variant({.level = StoryboardLevel::Event,
                                            .story = story_index,
                                            .act = act_index,
                                            .maneuver_group = group_index,
                                            .maneuver = maneuver_index,
                                            .event = event_index}));

            for (std::size_t action_index = 0; action_index < event.actions.size();
                 ++action_index) {
              auto* action_item =
                  new QTreeWidgetItem(event_item, {q(event.actions[action_index].name)});
              action_item->setData(0,
                                   Qt::UserRole,
                                   to_variant({.level = StoryboardLevel::Action,
                                               .story = story_index,
                                               .act = act_index,
                                               .maneuver_group = group_index,
                                               .maneuver = maneuver_index,
                                               .event = event_index,
                                               .action = action_index}));
            }
          }
        }
      }
    }
  }
  tree_->expandAll();
  populating_ = false;

  // BY PATH, never by row index: an index-based restore lands on a different
  // node the moment a sibling is inserted above it (the p6-s7 lesson).
  select_path(path_);
  emit storyboard_view_changed();
}

StoryboardPath StoryboardPanel::clamped(const StoryboardPath& path) const {
  const osc::Storyboard& storyboard = document_.scenario().storyboard;
  StoryboardPath out;
  if (path.level == StoryboardLevel::Root || path.story >= storyboard.stories.size()) {
    return out;
  }
  out = path;
  out.level = StoryboardLevel::Story;

  const osc::Story& story = storyboard.stories[path.story];
  if (path.level == StoryboardLevel::Story || path.act >= story.acts.size()) {
    return out;
  }
  out.level = StoryboardLevel::Act;

  const osc::Act& act = story.acts[path.act];
  if (path.level == StoryboardLevel::Act || path.maneuver_group >= act.maneuver_groups.size()) {
    return out;
  }
  out.level = StoryboardLevel::ManeuverGroup;

  const osc::ManeuverGroup& group = act.maneuver_groups[path.maneuver_group];
  if (path.level == StoryboardLevel::ManeuverGroup || path.maneuver >= group.maneuvers.size()) {
    return out;
  }
  out.level = StoryboardLevel::Maneuver;

  const osc::StoryManeuver& maneuver = group.maneuvers[path.maneuver];
  if (path.level == StoryboardLevel::Maneuver || path.event >= maneuver.events.size()) {
    return out;
  }
  out.level = StoryboardLevel::Event;

  const osc::Event& event = maneuver.events[path.event];
  if (path.level == StoryboardLevel::Event || path.action >= event.actions.size()) {
    return out;
  }
  out.level = StoryboardLevel::Action;
  return out;
}

void StoryboardPanel::select_path(const StoryboardPath& path) {
  path_ = clamped(path);

  populating_ = true;
  tree_->clearSelection();
  for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
    const StoryboardPath row = from_variant((*it)->data(0, Qt::UserRole));
    if (row.level == path_.level && row.story == path_.story && row.act == path_.act &&
        row.maneuver_group == path_.maneuver_group && row.maneuver == path_.maneuver &&
        row.event == path_.event && row.action == path_.action) {
      (*it)->setSelected(true);
      tree_->setCurrentItem(*it);
      break;
    }
  }
  populating_ = false;

  refresh_form();
}

// --- lookups ------------------------------------------------------------------

const osc::Story* StoryboardPanel::current_story() const {
  const osc::Storyboard& storyboard = document_.scenario().storyboard;
  if (path_.level == StoryboardLevel::Root || path_.story >= storyboard.stories.size()) {
    return nullptr;
  }
  return &storyboard.stories[path_.story];
}

osc::Event* StoryboardPanel::event_in(osc::Story& story) const {
  if (path_.level != StoryboardLevel::Event && path_.level != StoryboardLevel::Action) {
    return nullptr;
  }
  if (path_.act >= story.acts.size()) {
    return nullptr;
  }
  osc::Act& act = story.acts[path_.act];
  if (path_.maneuver_group >= act.maneuver_groups.size()) {
    return nullptr;
  }
  osc::ManeuverGroup& group = act.maneuver_groups[path_.maneuver_group];
  if (path_.maneuver >= group.maneuvers.size()) {
    return nullptr;
  }
  osc::StoryManeuver& maneuver = group.maneuvers[path_.maneuver];
  if (path_.event >= maneuver.events.size()) {
    return nullptr;
  }
  return &maneuver.events[path_.event];
}

osc::Action* StoryboardPanel::action_in(osc::Story& story) const {
  if (path_.level != StoryboardLevel::Action) {
    return nullptr;
  }
  osc::Event* event = event_in(story);
  if (event == nullptr || path_.action >= event->actions.size()) {
    return nullptr;
  }
  return &event->actions[path_.action];
}

bool StoryboardPanel::commit(const osc::Story& story) {
  if (path_.level == StoryboardLevel::Root) {
    return false;
  }
  const auto pushed = document_.push_scenario_command(
      osc::edit::set_story(document_.scenario(), path_.story, story));
  return pushed.has_value();
}

std::vector<QString> StoryboardPanel::phase_choices(const QString& controller_odr_id) const {
  const std::string wanted = controller_odr_id.toStdString();
  for (const osc::TrafficSignalController& controller :
       document_.scenario().road_network.traffic_signal_controllers) {
    if (controller.name != wanted) {
      continue;
    }
    // ★ `osc::phase_names`, NOT `Phase::name`. See this file's header.
    std::vector<QString> names;
    for (const std::string& name : osc::phase_names(controller)) {
      names.push_back(q(name));
    }
    return names;
  }
  return {};
}

// --- structural edits ----------------------------------------------------------

void StoryboardPanel::add_child() {
  const osc::Scenario& scenario = document_.scenario();
  const Seed seed = seed_from(scenario, selected_name());

  if (path_.level == StoryboardLevel::Root) {
    std::vector<std::string> taken;
    for (const osc::Story& story : scenario.storyboard.stories) {
      taken.push_back(story.name);
    }
    osc::Story story;
    story.name = unique_name("story", taken);
    story.acts.push_back(default_act(seed));

    const std::size_t index = scenario.storyboard.stories.size();
    if (document_.push_scenario_command(osc::edit::set_story(scenario, index, story)).has_value()) {
      path_ = {.level = StoryboardLevel::Story, .story = index};
      rebuild();
    }
    return;
  }

  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  StoryboardPath next = path_;

  const auto names_of = [](const auto& container) {
    std::vector<std::string> taken;
    for (const auto& entry : container) {
      taken.push_back(entry.name);
    }
    return taken;
  };

  switch (path_.level) {
  case StoryboardLevel::Story: {
    osc::Act act = default_act(seed);
    act.name = unique_name(act.name, names_of(story.acts));
    next.act = story.acts.size();
    next.level = StoryboardLevel::Act;
    story.acts.push_back(std::move(act));
    break;
  }
  case StoryboardLevel::Act: {
    if (path_.act >= story.acts.size()) {
      return;
    }
    osc::Act& act = story.acts[path_.act];
    osc::ManeuverGroup group = default_group(seed);
    group.name = unique_name(group.name, names_of(act.maneuver_groups));
    next.maneuver_group = act.maneuver_groups.size();
    next.level = StoryboardLevel::ManeuverGroup;
    act.maneuver_groups.push_back(std::move(group));
    break;
  }
  case StoryboardLevel::ManeuverGroup: {
    if (path_.act >= story.acts.size() ||
        path_.maneuver_group >= story.acts[path_.act].maneuver_groups.size()) {
      return;
    }
    osc::ManeuverGroup& group = story.acts[path_.act].maneuver_groups[path_.maneuver_group];
    osc::StoryManeuver maneuver = default_maneuver(seed);
    maneuver.name = unique_name(maneuver.name, names_of(group.maneuvers));
    next.maneuver = group.maneuvers.size();
    next.level = StoryboardLevel::Maneuver;
    group.maneuvers.push_back(std::move(maneuver));
    break;
  }
  case StoryboardLevel::Maneuver: {
    if (path_.act >= story.acts.size() ||
        path_.maneuver_group >= story.acts[path_.act].maneuver_groups.size()) {
      return;
    }
    osc::ManeuverGroup& group = story.acts[path_.act].maneuver_groups[path_.maneuver_group];
    if (path_.maneuver >= group.maneuvers.size()) {
      return;
    }
    osc::StoryManeuver& maneuver = group.maneuvers[path_.maneuver];
    osc::Event event = default_event(seed);
    event.name = unique_name(event.name, names_of(maneuver.events));
    next.event = maneuver.events.size();
    next.level = StoryboardLevel::Event;
    maneuver.events.push_back(std::move(event));
    break;
  }
  case StoryboardLevel::Event: {
    // A story <Action> wraps exactly one choice arm, so an EVENT is where a
    // second arm goes — which is what adding an action under an event means.
    osc::Event* event = event_in(story);
    if (event == nullptr) {
      return;
    }
    osc::Action action = default_action(ActionKind::LaneChange, seed);
    action.name = unique_name(action.name, names_of(event->actions));
    next.action = event->actions.size();
    next.level = StoryboardLevel::Action;
    event->actions.push_back(std::move(action));
    break;
  }
  case StoryboardLevel::Action:
  case StoryboardLevel::Root:
    return; // an action has no child
  }

  if (commit(story)) {
    path_ = next;
    rebuild();
  }
}

void StoryboardPanel::remove_selected() {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }

  if (path_.level == StoryboardLevel::Story) {
    if (document_.push_scenario_command(osc::edit::remove_story(document_.scenario(), path_.story))
            .has_value()) {
      path_ = {};
      rebuild();
    }
    return;
  }

  osc::Story story = *current;
  StoryboardPath next = path_;

  // ★ REMOVING THE LAST CHILD REMOVES THE PARENT. Every level below <Story> has
  // a schema minimum of one, so leaving an empty act behind would produce a
  // document write_xosc refuses — a deletion that appeared to succeed and then
  // made the scenario unsavable (the remove_route_waypoint lesson).
  switch (path_.level) {
  case StoryboardLevel::Act: {
    if (path_.act >= story.acts.size()) {
      return;
    }
    if (story.acts.size() == 1) {
      // The story's last act: remove the story instead.
      if (document_
              .push_scenario_command(osc::edit::remove_story(document_.scenario(), path_.story))
              .has_value()) {
        path_ = {};
        rebuild();
      }
      return;
    }
    story.acts.erase(story.acts.begin() + static_cast<std::ptrdiff_t>(path_.act));
    next = {.level = StoryboardLevel::Story, .story = path_.story};
    break;
  }
  case StoryboardLevel::ManeuverGroup: {
    if (path_.act >= story.acts.size()) {
      return;
    }
    osc::Act& act = story.acts[path_.act];
    if (path_.maneuver_group >= act.maneuver_groups.size()) {
      return;
    }
    if (act.maneuver_groups.size() == 1) {
      if (story.acts.size() == 1) {
        if (document_
                .push_scenario_command(osc::edit::remove_story(document_.scenario(), path_.story))
                .has_value()) {
          path_ = {};
          rebuild();
        }
        return;
      }
      story.acts.erase(story.acts.begin() + static_cast<std::ptrdiff_t>(path_.act));
      next = {.level = StoryboardLevel::Story, .story = path_.story};
      break;
    }
    act.maneuver_groups.erase(act.maneuver_groups.begin() +
                              static_cast<std::ptrdiff_t>(path_.maneuver_group));
    next = {.level = StoryboardLevel::Act, .story = path_.story, .act = path_.act};
    break;
  }
  case StoryboardLevel::Maneuver: {
    if (path_.act >= story.acts.size() ||
        path_.maneuver_group >= story.acts[path_.act].maneuver_groups.size()) {
      return;
    }
    osc::ManeuverGroup& group = story.acts[path_.act].maneuver_groups[path_.maneuver_group];
    if (path_.maneuver >= group.maneuvers.size()) {
      return;
    }
    // A <ManeuverGroup> may legally have NO maneuver (minOccurs 0), unlike its
    // siblings — so this is the one level that simply shrinks.
    group.maneuvers.erase(group.maneuvers.begin() + static_cast<std::ptrdiff_t>(path_.maneuver));
    next = {.level = StoryboardLevel::ManeuverGroup,
            .story = path_.story,
            .act = path_.act,
            .maneuver_group = path_.maneuver_group};
    break;
  }
  case StoryboardLevel::Event: {
    if (path_.act >= story.acts.size() ||
        path_.maneuver_group >= story.acts[path_.act].maneuver_groups.size()) {
      return;
    }
    osc::ManeuverGroup& group = story.acts[path_.act].maneuver_groups[path_.maneuver_group];
    if (path_.maneuver >= group.maneuvers.size()) {
      return;
    }
    osc::StoryManeuver& maneuver = group.maneuvers[path_.maneuver];
    if (path_.event >= maneuver.events.size()) {
      return;
    }
    if (maneuver.events.size() == 1) {
      group.maneuvers.erase(group.maneuvers.begin() + static_cast<std::ptrdiff_t>(path_.maneuver));
      next = {.level = StoryboardLevel::ManeuverGroup,
              .story = path_.story,
              .act = path_.act,
              .maneuver_group = path_.maneuver_group};
      break;
    }
    maneuver.events.erase(maneuver.events.begin() + static_cast<std::ptrdiff_t>(path_.event));
    next = {.level = StoryboardLevel::Maneuver,
            .story = path_.story,
            .act = path_.act,
            .maneuver_group = path_.maneuver_group,
            .maneuver = path_.maneuver};
    break;
  }
  case StoryboardLevel::Action: {
    osc::Event* event = event_in(story);
    if (event == nullptr || path_.action >= event->actions.size()) {
      return;
    }
    if (event->actions.size() == 1) {
      // The event's last action: remove the event, recursing through the same
      // rule one level up rather than duplicating it.
      const StoryboardPath saved = path_;
      path_.level = StoryboardLevel::Event;
      remove_selected();
      if (path_.level == StoryboardLevel::Action) {
        path_ = saved; // the removal was refused; leave the selection alone
      }
      return;
    }
    event->actions.erase(event->actions.begin() + static_cast<std::ptrdiff_t>(path_.action));
    next = {.level = StoryboardLevel::Event,
            .story = path_.story,
            .act = path_.act,
            .maneuver_group = path_.maneuver_group,
            .maneuver = path_.maneuver,
            .event = path_.event};
    break;
  }
  case StoryboardLevel::Story:
  case StoryboardLevel::Root:
    return;
  }

  if (commit(story)) {
    path_ = next;
    rebuild();
  }
}

// --- field edits ----------------------------------------------------------------

void StoryboardPanel::set_selected_name(const QString& name) {
  const osc::Story* current = current_story();
  if (current == nullptr || name.isEmpty()) {
    return;
  }
  osc::Story story = *current;
  const std::string next = name.toStdString();

  switch (path_.level) {
  case StoryboardLevel::Story:
    if (story.name == next) {
      return;
    }
    story.name = next;
    break;
  case StoryboardLevel::Act:
    if (path_.act >= story.acts.size() || story.acts[path_.act].name == next) {
      return;
    }
    story.acts[path_.act].name = next;
    break;
  case StoryboardLevel::ManeuverGroup: {
    if (path_.act >= story.acts.size()) {
      return;
    }
    osc::Act& act = story.acts[path_.act];
    if (path_.maneuver_group >= act.maneuver_groups.size() ||
        act.maneuver_groups[path_.maneuver_group].name == next) {
      return;
    }
    act.maneuver_groups[path_.maneuver_group].name = next;
    break;
  }
  case StoryboardLevel::Maneuver: {
    if (path_.act >= story.acts.size() ||
        path_.maneuver_group >= story.acts[path_.act].maneuver_groups.size()) {
      return;
    }
    osc::ManeuverGroup& group = story.acts[path_.act].maneuver_groups[path_.maneuver_group];
    if (path_.maneuver >= group.maneuvers.size() || group.maneuvers[path_.maneuver].name == next) {
      return;
    }
    group.maneuvers[path_.maneuver].name = next;
    break;
  }
  case StoryboardLevel::Event: {
    osc::Event* event = event_in(story);
    if (event == nullptr || event->name == next) {
      return;
    }
    event->name = next;
    break;
  }
  case StoryboardLevel::Action: {
    osc::Action* action = action_in(story);
    if (action == nullptr || action->name == next) {
      return;
    }
    action->name = next;
    break;
  }
  case StoryboardLevel::Root:
    return;
  }

  if (commit(story)) {
    rebuild();
  }
}

void StoryboardPanel::set_event_priority(const QString& priority) {
  const osc::Story* current = current_story();
  if (current == nullptr || priority.isEmpty()) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = event_in(story);
  if (event == nullptr || event->priority == priority.toStdString()) {
    return;
  }
  event->priority = priority.toStdString();
  commit_field(story);
}

void StoryboardPanel::set_group_actors(const std::vector<QString>& entity_names) {
  const osc::Story* current = current_story();
  if (current == nullptr || path_.level != StoryboardLevel::ManeuverGroup) {
    return;
  }
  osc::Story story = *current;
  if (path_.act >= story.acts.size()) {
    return;
  }
  osc::Act& act = story.acts[path_.act];
  if (path_.maneuver_group >= act.maneuver_groups.size()) {
    return;
  }
  std::vector<osc::EntityRef> refs;
  for (const QString& name : entity_names) {
    refs.push_back(osc::EntityRef{.entity_ref = name.toStdString(), .preserved = {}});
  }
  act.maneuver_groups[path_.maneuver_group].actors = std::move(refs);
  commit_field(story);
}

void StoryboardPanel::set_action_kind(ActionKind kind) {
  const osc::Story* current = current_story();
  if (current == nullptr || selected_action_kind() == kind) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  osc::Action replacement = default_action(kind, seed_from(document_.scenario(), selected_name()));
  // The NAME survives a retype — it is the user's label for the action, not
  // part of what the action does.
  replacement.name = action->name;
  *action = std::move(replacement);
  if (commit(story)) {
    refresh_form();
  }
}

void StoryboardPanel::set_action_entity(const QString& entity_name) {
  const osc::Story* current = current_story();
  if (current == nullptr || entity_name.isEmpty()) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  auto* entry = std::get_if<osc::PrivateAction>(&action->action);
  if (entry == nullptr || !entry->lateral.has_value() || !entry->lateral->lane_change.has_value()) {
    return;
  }
  auto* relative = std::get_if<osc::RelativeTargetLane>(&entry->lateral->lane_change->target);
  if (relative == nullptr || relative->entity_ref == entity_name.toStdString()) {
    return;
  }
  relative->entity_ref = entity_name.toStdString();
  commit_field(story);
}

void StoryboardPanel::set_action_value(double value) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  auto* entry = std::get_if<osc::PrivateAction>(&action->action);
  if (entry == nullptr) {
    return;
  }
  if (entry->lateral.has_value() && entry->lateral->lane_change.has_value()) {
    // Lanes to move: an INT, so a spin box showing 1.4 lanes means 1.
    auto* relative = std::get_if<osc::RelativeTargetLane>(&entry->lateral->lane_change->target);
    if (relative == nullptr) {
      return;
    }
    const int lanes = static_cast<int>(value);
    if (relative->value == lanes) {
      return;
    }
    relative->value = lanes;
  } else if (entry->longitudinal.has_value() && entry->longitudinal->speed.has_value() &&
             entry->longitudinal->speed->absolute_target.has_value()) {
    if (entry->longitudinal->speed->absolute_target->value == value) {
      return;
    }
    entry->longitudinal->speed->absolute_target->value = value;
  } else {
    return;
  }
  commit_field(story);
}

void StoryboardPanel::set_action_signal_id(const QString& odr_id) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  auto* global = std::get_if<osc::GlobalAction>(&action->action);
  if (global == nullptr || !global->infrastructure.has_value()) {
    return;
  }
  auto* state =
      std::get_if<osc::TrafficSignalStateAction>(&global->infrastructure->traffic_signal.action);
  if (state == nullptr || state->name == odr_id.toStdString()) {
    return;
  }
  state->name = odr_id.toStdString();
  commit_field(story);
}

void StoryboardPanel::set_action_state(const QString& state_text) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  auto* global = std::get_if<osc::GlobalAction>(&action->action);
  if (global == nullptr || !global->infrastructure.has_value()) {
    return;
  }
  auto* state =
      std::get_if<osc::TrafficSignalStateAction>(&global->infrastructure->traffic_signal.action);
  if (state == nullptr || state->state == state_text.toStdString()) {
    return;
  }
  state->state = state_text.toStdString();
  commit_field(story);
}

void StoryboardPanel::set_action_controller(const QString& controller_odr_id,
                                            const QString& phase) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Action* action = action_in(story);
  if (action == nullptr) {
    return;
  }
  auto* global = std::get_if<osc::GlobalAction>(&action->action);
  if (global == nullptr || !global->infrastructure.has_value()) {
    return;
  }
  auto* controller = std::get_if<osc::TrafficSignalControllerAction>(
      &global->infrastructure->traffic_signal.action);
  if (controller == nullptr) {
    return;
  }
  if (controller->traffic_signal_controller_ref == controller_odr_id.toStdString() &&
      controller->phase == phase.toStdString()) {
    return;
  }
  controller->traffic_signal_controller_ref = controller_odr_id.toStdString();
  controller->phase = phase.toStdString();
  commit_field(story);
}

// --- condition edits --------------------------------------------------------------

namespace {

/// The event a condition edit applies to: the selected event, or the event that
/// owns the selected action — so the trigger form keeps working while the user
/// has an action selected.
osc::Event* trigger_event(osc::Story& story, const StoryboardPath& path) {
  if (path.act >= story.acts.size()) {
    return nullptr;
  }
  osc::Act& act = story.acts[path.act];
  if (path.maneuver_group >= act.maneuver_groups.size()) {
    return nullptr;
  }
  osc::ManeuverGroup& group = act.maneuver_groups[path.maneuver_group];
  if (path.maneuver >= group.maneuvers.size()) {
    return nullptr;
  }
  osc::StoryManeuver& maneuver = group.maneuvers[path.maneuver];
  if (path.event >= maneuver.events.size()) {
    return nullptr;
  }
  return &maneuver.events[path.event];
}

} // namespace

void StoryboardPanel::set_condition_kind(ConditionKind kind) {
  const osc::Story* current = current_story();
  if (current == nullptr || selected_condition_kind() == kind) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }

  if (kind == ConditionKind::None) {
    // The TRIGGER goes, not just the condition: an empty <StartTrigger> is an
    // element the file did not have, and "no trigger" is its own legal state.
    event->start_trigger.reset();
  } else {
    osc::Condition condition =
        default_condition(kind, seed_from(document_.scenario(), q(event->name)));
    condition.name = event->name + "_start";
    if (!event->start_trigger.has_value()) {
      event->start_trigger = osc::Trigger{};
    }
    if (event->start_trigger->condition_groups.empty()) {
      event->start_trigger->condition_groups.push_back(osc::ConditionGroup{});
    }
    if (event->start_trigger->condition_groups[0].conditions.empty()) {
      event->start_trigger->condition_groups[0].conditions.push_back(std::move(condition));
    } else {
      event->start_trigger->condition_groups[0].conditions[0] = std::move(condition);
    }
  }

  if (commit(story)) {
    refresh_form();
  }
}

void StoryboardPanel::set_condition_entity(const QString& entity_name) {
  const osc::Story* current = current_story();
  if (current == nullptr || entity_name.isEmpty()) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }
  osc::Condition* condition = first_condition(*event);
  if (condition == nullptr || !condition->by_entity.has_value()) {
    return;
  }
  osc::ByEntityCondition& by_entity = *condition->by_entity;
  by_entity.triggering_entities.entity_refs.assign(
      1, osc::EntityRef{.entity_ref = entity_name.toStdString(), .preserved = {}});
  if (auto* distance = std::get_if<osc::RelativeDistanceCondition>(&by_entity.entity_condition)) {
    // The MEASURED-AGAINST entity is the other one; leaving it equal to the
    // triggering entity measures a distance from a car to itself.
    if (distance->entity_ref == entity_name.toStdString()) {
      for (const osc::ScenarioObject& object : document_.scenario().entities.scenario_objects) {
        if (object.name != entity_name.toStdString()) {
          distance->entity_ref = object.name;
          break;
        }
      }
    }
  }
  commit_field(story);
}

void StoryboardPanel::set_condition_value(double value) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }
  osc::Condition* condition = first_condition(*event);
  if (condition == nullptr) {
    return;
  }
  if (condition->simulation_time.has_value()) {
    condition->simulation_time->value = value;
  } else if (condition->by_entity.has_value()) {
    if (auto* distance =
            std::get_if<osc::RelativeDistanceCondition>(&condition->by_entity->entity_condition)) {
      distance->value = value;
    } else if (auto* speed =
                   std::get_if<osc::SpeedCondition>(&condition->by_entity->entity_condition)) {
      speed->value = value;
    } else {
      return;
    }
  } else {
    return;
  }
  commit_field(story);
}

void StoryboardPanel::set_condition_rule(const QString& rule) {
  const osc::Story* current = current_story();
  if (current == nullptr || rule.isEmpty()) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }
  osc::Condition* condition = first_condition(*event);
  if (condition == nullptr) {
    return;
  }
  const std::string next = rule.toStdString();
  if (condition->simulation_time.has_value()) {
    condition->simulation_time->rule = next;
  } else if (condition->by_entity.has_value()) {
    if (auto* distance =
            std::get_if<osc::RelativeDistanceCondition>(&condition->by_entity->entity_condition)) {
      distance->rule = next;
    } else if (auto* speed =
                   std::get_if<osc::SpeedCondition>(&condition->by_entity->entity_condition)) {
      speed->rule = next;
    } else {
      return;
    }
  } else {
    return;
  }
  commit_field(story);
}

void StoryboardPanel::set_condition_state(const QString& state) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }
  osc::Condition* condition = first_condition(*event);
  if (condition == nullptr) {
    return;
  }
  if (condition->traffic_signal.has_value()) {
    condition->traffic_signal->state = state.toStdString();
  } else if (condition->storyboard_element_state.has_value()) {
    condition->storyboard_element_state->state = state.toStdString();
  } else {
    return;
  }
  commit_field(story);
}

void StoryboardPanel::set_condition_controller(const QString& controller_odr_id,
                                               const QString& phase) {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return;
  }
  osc::Story story = *current;
  osc::Event* event = trigger_event(story, path_);
  if (event == nullptr) {
    return;
  }
  osc::Condition* condition = first_condition(*event);
  if (condition == nullptr) {
    return;
  }
  if (condition->traffic_signal_controller.has_value()) {
    condition->traffic_signal_controller->traffic_signal_controller_ref =
        controller_odr_id.toStdString();
    condition->traffic_signal_controller->phase = phase.toStdString();
  } else if (condition->traffic_signal.has_value()) {
    condition->traffic_signal->name = controller_odr_id.toStdString();
  } else {
    return;
  }
  commit_field(story);
}

// --- getters and the form ---------------------------------------------------------

ActionKind StoryboardPanel::selected_action_kind() const {
  const osc::Story* current = current_story();
  if (current == nullptr || path_.level != StoryboardLevel::Action) {
    return ActionKind::Preserved;
  }
  osc::Story copy = *current;
  const osc::Action* action = action_in(copy);
  if (action == nullptr) {
    return ActionKind::Preserved;
  }
  if (const auto* entry = std::get_if<osc::PrivateAction>(&action->action)) {
    if (entry->lateral.has_value()) {
      return ActionKind::LaneChange;
    }
    if (entry->longitudinal.has_value()) {
      return ActionKind::Speed;
    }
    return ActionKind::Preserved;
  }
  if (const auto* global = std::get_if<osc::GlobalAction>(&action->action)) {
    if (!global->infrastructure.has_value()) {
      return ActionKind::Preserved;
    }
    const auto& arm = global->infrastructure->traffic_signal.action;
    if (std::holds_alternative<osc::TrafficSignalStateAction>(arm)) {
      return ActionKind::TrafficSignalState;
    }
    if (std::holds_alternative<osc::TrafficSignalControllerAction>(arm)) {
      return ActionKind::TrafficSignalPhase;
    }
  }
  return ActionKind::Preserved;
}

ConditionKind StoryboardPanel::selected_condition_kind() const {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return ConditionKind::None;
  }
  osc::Story copy = *current;
  const osc::Event* event = trigger_event(copy, path_);
  if (event == nullptr) {
    return ConditionKind::None;
  }
  const osc::Condition* condition = first_condition(*event);
  if (condition == nullptr) {
    return ConditionKind::None;
  }
  if (condition->simulation_time.has_value()) {
    return ConditionKind::SimulationTime;
  }
  if (condition->traffic_signal.has_value()) {
    return ConditionKind::TrafficSignalState;
  }
  if (condition->traffic_signal_controller.has_value()) {
    return ConditionKind::TrafficSignalPhase;
  }
  if (condition->storyboard_element_state.has_value()) {
    return ConditionKind::StoryboardElementState;
  }
  if (condition->by_entity.has_value()) {
    if (std::holds_alternative<osc::RelativeDistanceCondition>(
            condition->by_entity->entity_condition)) {
      return ConditionKind::RelativeDistance;
    }
    if (std::holds_alternative<osc::SpeedCondition>(condition->by_entity->entity_condition)) {
      return ConditionKind::Speed;
    }
  }
  return ConditionKind::Preserved;
}

QString StoryboardPanel::selected_name() const {
  const osc::Story* current = current_story();
  if (current == nullptr) {
    return {};
  }
  osc::Story copy = *current;
  switch (path_.level) {
  case StoryboardLevel::Story:
    return q(copy.name);
  case StoryboardLevel::Act:
    return path_.act < copy.acts.size() ? q(copy.acts[path_.act].name) : QString();
  case StoryboardLevel::ManeuverGroup: {
    if (path_.act >= copy.acts.size()) {
      return {};
    }
    const osc::Act& act = copy.acts[path_.act];
    return path_.maneuver_group < act.maneuver_groups.size()
               ? q(act.maneuver_groups[path_.maneuver_group].name)
               : QString();
  }
  case StoryboardLevel::Maneuver: {
    if (path_.act >= copy.acts.size() ||
        path_.maneuver_group >= copy.acts[path_.act].maneuver_groups.size()) {
      return {};
    }
    const osc::ManeuverGroup& group = copy.acts[path_.act].maneuver_groups[path_.maneuver_group];
    return path_.maneuver < group.maneuvers.size() ? q(group.maneuvers[path_.maneuver].name)
                                                   : QString();
  }
  case StoryboardLevel::Event: {
    const osc::Event* event = event_in(copy);
    return event != nullptr ? q(event->name) : QString();
  }
  case StoryboardLevel::Action: {
    const osc::Action* action = action_in(copy);
    return action != nullptr ? q(action->name) : QString();
  }
  case StoryboardLevel::Root:
    return {};
  }
  return {};
}

void StoryboardPanel::refresh_form() {
  populating_ = true;

  const osc::Scenario& scenario = document_.scenario();
  const bool has_node = path_.level != StoryboardLevel::Root;
  const bool is_event_or_action =
      path_.level == StoryboardLevel::Event || path_.level == StoryboardLevel::Action;
  const bool is_action = path_.level == StoryboardLevel::Action;

  empty_label_->setVisible(!has_node);
  name_edit_->setEnabled(has_node);
  priority_combo_->setEnabled(path_.level == StoryboardLevel::Event);
  action_kind_combo_->setEnabled(is_action);
  condition_kind_combo_->setEnabled(is_event_or_action);

  name_edit_->setText(selected_name());

  entity_combo_->clear();
  for (const osc::ScenarioObject& object : scenario.entities.scenario_objects) {
    entity_combo_->addItem(q(object.name));
  }
  controller_combo_->clear();
  for (const osc::TrafficSignalController& controller :
       scenario.road_network.traffic_signal_controllers) {
    controller_combo_->addItem(q(controller.name));
  }
  phase_combo_->clear();
  for (const QString& phase : phase_choices(controller_combo_->currentText())) {
    phase_combo_->addItem(phase);
  }

  const ActionKind action_kind = selected_action_kind();
  const int action_index = action_kind_combo_->findData(static_cast<int>(action_kind));
  action_kind_combo_->setCurrentIndex(action_index >= 0 ? action_index : 0);

  const ConditionKind condition_kind = selected_condition_kind();
  const int condition_index = condition_kind_combo_->findData(static_cast<int>(condition_kind));
  condition_kind_combo_->setCurrentIndex(condition_index >= 0 ? condition_index : 0);

  // Fields apply to whichever of the two the form is showing; a field that
  // applies to neither is disabled rather than hidden, so the layout does not
  // jump as the selection moves down the tree.
  const bool wants_entity = (is_action && action_kind == ActionKind::LaneChange) ||
                            (!is_action && (condition_kind == ConditionKind::RelativeDistance ||
                                            condition_kind == ConditionKind::Speed));
  const bool wants_controller =
      (is_action && (action_kind == ActionKind::TrafficSignalPhase ||
                     action_kind == ActionKind::TrafficSignalState)) ||
      (!is_action && (condition_kind == ConditionKind::TrafficSignalPhase ||
                      condition_kind == ConditionKind::TrafficSignalState));
  entity_combo_->setEnabled(wants_entity);
  controller_combo_->setEnabled(wants_controller);
  phase_combo_->setEnabled(wants_controller);
  state_edit_->setEnabled(wants_controller ||
                          condition_kind == ConditionKind::StoryboardElementState);
  rule_combo_->setEnabled(!is_action && condition_kind != ConditionKind::None);
  value_spin_->setEnabled(has_node);

  populating_ = false;
}

} // namespace roadmaker::editor
