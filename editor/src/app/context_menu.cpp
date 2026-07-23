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

#include "app/context_menu.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/network.hpp"

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/actions.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

void select_road(const ContextMenuDeps& deps, RoadId road) {
  deps.selection.select({.road = road, .lane = LaneId{}}, SelectMode::Replace);
}

void select_object(const ContextMenuDeps& deps, RoadId road, ObjectId object) {
  deps.selection.select({.road = road, .lane = LaneId{}, .object = object}, SelectMode::Replace);
}

/// Lowest positive integer odr id not already used by an object — keeps a
/// duplicated prop id_unique_in_class-valid.
std::string next_object_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

MenuItem separator() {
  return MenuItem{.separator = true};
}

/// Adds `items` to `menu`, recursing into submenus. Split out of
/// assemble_context_menu so nesting costs one call rather than a second loop.
void fill_menu(QMenu& menu, const std::vector<MenuItem>& items) {
  for (const MenuItem& item : items) {
    if (item.separator) {
      menu.addSeparator();
      continue;
    }
    if (!item.children.empty()) {
      QMenu* submenu = menu.addMenu(item.text);
      submenu->setEnabled(item.enabled);
      fill_menu(*submenu, item.children);
      continue;
    }
    QAction* action = menu.addAction(item.text);
    action->setEnabled(item.enabled);
    if (item.invoke) {
      QObject::connect(action, &QAction::triggered, &menu, [invoke = item.invoke] { invoke(); });
    }
  }
}

/// The first free end-pair of two selected roads that edit::close_gap can weld
/// (the four contact combinations), or nullopt when none is linkable — drives
/// the "Link Ends" item (finding 3).
std::optional<std::pair<RoadEnd, RoadEnd>>
linkable_ends(const RoadNetwork& network, RoadId a, RoadId b) {
  for (const ContactPoint ca : {ContactPoint::Start, ContactPoint::End}) {
    for (const ContactPoint cb : {ContactPoint::Start, ContactPoint::End}) {
      const RoadEnd ea{.road = a, .contact = ca};
      const RoadEnd eb{.road = b, .contact = cb};
      if (edit::check_linkable(network, ea, eb).has_value()) {
        return std::pair{ea, eb};
      }
    }
  }
  return std::nullopt;
}

/// A lane is removable exactly when the kernel's edit::remove_lane accepts it:
/// non-center and the outermost lane of its side (M2 restriction). Mirrored
/// here so the menu can enable/disable the item; the kernel stays the final
/// arbiter (a refused command appends a diagnostic).
bool lane_removable(const RoadNetwork& network, LaneId lane_id) {
  const Lane* lane = network.lane(lane_id);
  if (lane == nullptr || lane->odr_id == 0) {
    return false;
  }
  const LaneSection* section = network.lane_section(lane->section);
  if (section == nullptr) {
    return false;
  }
  for (const LaneId other_id : section->lanes) {
    const int other = network.lane(other_id)->odr_id;
    if ((lane->odr_id > 0 && other > lane->odr_id) || (lane->odr_id < 0 && other < lane->odr_id)) {
      return false; // a lane sits further out — not the outermost
    }
  }
  return true;
}

/// The four DERIVED junction states (p4-s4, issue #319). Never stored as an
/// enum on Junction — read off `arms`/`spans`/`locked` exactly as
/// road/junction.hpp documents, so the menu and the kernel cannot drift.
enum class JunctionKind {
  Foreign,   ///< no arms, no spans (read from someone else's file)
  Automatic, ///< arms, unlocked — the regeneration loop owns it
  Locked,    ///< arms, locked — hand-tuned, membership editable
  Span,      ///< §12.7 virtual junction; structurally locked forever
};

JunctionKind junction_kind(const Junction& junction) {
  if (!junction.spans.empty()) {
    return JunctionKind::Span;
  }
  if (junction.arms.empty()) {
    return JunctionKind::Foreign;
  }
  return junction.locked ? JunctionKind::Locked : JunctionKind::Automatic;
}

/// Mirrors edit::add_junction_arm's preconditions so the node menu can OFFER
/// the item only when it would succeed (the kernel stays the final arbiter):
/// a live, arm-based, LOCKED junction, an end it does not already own, an end
/// no other junction owns, and a free link slot on that end.
bool can_add_arm(const RoadNetwork& network, JunctionId junction_id, const RoadEnd& end) {
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr || junction_kind(*junction) != JunctionKind::Locked) {
    return false;
  }
  if (std::ranges::find(junction->arms, end) != junction->arms.end()) {
    return false;
  }
  if (edit::junction_at_end(network, end).has_value()) {
    return false;
  }
  const Road* road = network.road(end.road);
  if (road == nullptr || road->junction.is_valid()) {
    return false;
  }
  const auto& slot = end.contact == ContactPoint::Start ? road->predecessor : road->successor;
  return !slot.has_value();
}

/// The road END a waypoint index names, or nullopt for an interior node.
std::optional<RoadEnd> end_for_node(const RoadNetwork& network, RoadId road_id, std::size_t index) {
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return std::nullopt;
  }
  const auto stations = edit::waypoint_stations(*road);
  if (!stations.has_value() || stations->size() < 2 || index >= stations->size()) {
    return std::nullopt;
  }
  if (index == 0) {
    return RoadEnd{.road = road_id, .contact = ContactPoint::Start};
  }
  if (index + 1 == stations->size()) {
    return RoadEnd{.road = road_id, .contact = ContactPoint::End};
  }
  return std::nullopt;
}

QString junction_name(const RoadNetwork& network, JunctionId id) {
  const Junction* junction = network.junction(id);
  return junction != nullptr ? QString::fromStdString(junction->odr_id) : QStringLiteral("?");
}

} // namespace

MenuContext menu_context_for_pick(const RoadNetwork& network, const std::optional<PickHit>& hit) {
  MenuContext context;
  if (!hit.has_value()) {
    return context;
  }
  context.pick = hit;
  if (hit->junction.is_valid()) {
    context.junction = hit->junction;
  }
  if (const Road* road = network.road(hit->road)) {
    context.station = find_station(road->plan_view, hit->position[0], hit->position[1]).s;
  }
  return context;
}

std::vector<MenuItem> build_context_menu(const MenuContext& context, ContextMenuDeps& deps) {
  std::vector<MenuItem> items;
  const RoadNetwork& network = deps.document.network();

  // Node menu — highest priority (a node sits on a road body).
  if (context.node.has_value()) {
    const RoadId road = context.node->road;
    const std::size_t index = context.node->index;
    std::optional<double> station;
    bool interior = false;
    if (const Road* road_ptr = network.road(road)) {
      if (const auto stations = edit::waypoint_stations(*road_ptr);
          stations.has_value() && index < stations->size()) {
        station = stations->at(index);
        interior = index > 0 && index + 1 < stations->size();
      }
    }
    items.push_back(MenuItem{.text = QObject::tr("Split at this node"),
                             .enabled = interior,
                             .invoke = [deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::split_road(deps.document.network(), road, *station));
                               }
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Delete node"), .invoke = [deps, road, index] {
                               (void)deps.document.push_command(
                                   edit::delete_waypoint(deps.document.network(), road, index));
                             }});
    // Membership: hand THIS road end to the one selected LOCKED junction
    // (p4-s4, issue #319). Offered only when edit::add_junction_arm would
    // accept it, so the item never appears as a trap — an automatic junction
    // must be locked first (its arms are re-derived from the roads that meet
    // it, so an edit to them would not survive).
    const std::vector<JunctionId> selected_junctions = deps.selection.selected_junctions();
    if (selected_junctions.size() == 1) {
      const JunctionId target = selected_junctions.front();
      if (const std::optional<RoadEnd> end = end_for_node(network, road, index);
          end.has_value() && can_add_arm(network, target, *end)) {
        items.push_back(MenuItem{
            .text = QObject::tr("Add end to junction %1").arg(junction_name(network, target)),
            .invoke = [deps, target, end = *end] {
              (void)deps.document.push_command(
                  edit::add_junction_arm(deps.document.network(), target, end));
            }});
      }
    }
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.frame_selection->trigger();
                             }});
    return items;
  }

  // Ground-surface menu (p5-s1, issue #231). Placed before the junction and
  // road branches: a surface pick carries no road, and its own boundary items
  // are the only thing there is to offer on it.
  if (context.pick.has_value() && context.pick->surface.is_valid()) {
    const SurfaceId surface_id = context.pick->surface;
    const Surface* surface = network.surface(surface_id);
    const bool authored = surface != nullptr && surface->source == BoundarySource::Authored;
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps, surface_id] {
                               deps.selection.select({.surface = surface_id}, SelectMode::Replace);
                               deps.actions.frame_selection->trigger();
                             }});
    // Arm the Surface tool with this surface selected — the same discoverable
    // "menu hands you the tool" path Move uses for roads and props.
    items.push_back(MenuItem{.text = QObject::tr("Edit boundary"), .invoke = [deps, surface_id] {
                               deps.selection.select({.surface = surface_id}, SelectMode::Replace);
                               deps.actions.tool_surface->trigger();
                             }});
    items.push_back(separator());
    // Only offered once an edit has detached the surface (decision D3) — on a
    // derived one there is nothing to revert.
    items.push_back(MenuItem{.text = QObject::tr("Revert to derived"),
                             .enabled = authored,
                             .invoke = [deps, surface_id] {
                               (void)deps.document.push_command(edit::revert_surface_to_derived(
                                   deps.document.network(), surface_id));
                             }});
    return items;
  }

  // Junction menu (reached from the scene tree — junction floors aren't picked
  // in the viewport).
  if (context.junction.has_value()) {
    const JunctionId junction = *context.junction;
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps] {
                               deps.selection.clear();
                               deps.actions.frame_selection->trigger();
                             }});
    items.push_back(separator());

    // Junction control (p4-s4, issue #319). The state is DERIVED, so the
    // items read it off the record rather than off a stored mode.
    const Junction* junction_ptr = network.junction(junction);
    const JunctionKind kind =
        junction_ptr != nullptr ? junction_kind(*junction_ptr) : JunctionKind::Foreign;
    // A span junction is locked structurally (§12.7 virtual junctions are never
    // derived) and a foreign junction has no derivation to guard, so only the
    // two arm-based states can toggle — the same rule set_junction_locked
    // enforces.
    const bool arm_based = kind == JunctionKind::Automatic || kind == JunctionKind::Locked;
    const bool currently_locked = kind == JunctionKind::Locked || kind == JunctionKind::Span;
    items.push_back(MenuItem{
        .text = currently_locked ? QObject::tr("Unlock junction") : QObject::tr("Lock junction"),
        .enabled = arm_based,
        .invoke = [deps, junction, currently_locked] {
          (void)deps.document.push_command(
              edit::set_junction_locked(deps.document.network(), junction, !currently_locked));
        }});
    // Plain regenerate_junction: the lock is a policy of the AUTOMATIC loops
    // only, so an explicit re-derive needs no bypass flag. Refused for the two
    // arm-less states (there is nothing to re-run the generator from).
    items.push_back(MenuItem{.text = QObject::tr("Re-derive junction"),
                             .enabled = arm_based,
                             .invoke = [deps, junction] {
                               (void)deps.document.push_command(
                                   edit::regenerate_junction(deps.document.network(), junction));
                             }});
    // Per-arm removal — locked junctions only (membership is meaningful only
    // once the user has taken the junction out of the automatic loop), and only
    // while a third arm keeps the remainder above the kernel's 2-arm floor.
    if (kind == JunctionKind::Locked && junction_ptr->arms.size() >= 3) {
      for (const RoadEnd& arm : junction_ptr->arms) {
        const Road* arm_road = network.road(arm.road);
        if (arm_road == nullptr) {
          continue;
        }
        items.push_back(MenuItem{
            .text = QObject::tr("Remove arm: %1").arg(QString::fromStdString(arm_road->odr_id)),
            .invoke = [deps, junction, arm] {
              (void)deps.document.push_command(
                  edit::remove_junction_arm(deps.document.network(), junction, arm));
            }});
      }
    }
    // Merge — the two-selected gating merge_roads established. The survivor is
    // the FIRST-selected junction, named in the text so the convention needs no
    // documentation lookup.
    const std::vector<JunctionId> selected_junctions = deps.selection.selected_junctions();
    const auto arm_mergeable = [&network](JunctionId id) {
      const Junction* candidate = network.junction(id);
      return candidate != nullptr && candidate->spans.empty() && candidate->arms.size() >= 2;
    };
    const bool junctions_mergeable =
        selected_junctions.size() == 2 && selected_junctions[0] != selected_junctions[1] &&
        arm_mergeable(selected_junctions[0]) && arm_mergeable(selected_junctions[1]);
    items.push_back(MenuItem{
        .text = junctions_mergeable ? QObject::tr("Merge selected junctions into %1")
                                          .arg(junction_name(network, selected_junctions[0]))
                                    : QObject::tr("Merge selected junctions"),
        .enabled = junctions_mergeable,
        .invoke = [deps, selected_junctions] {
          if (selected_junctions.size() != 2) {
            return;
          }
          (void)deps.document.push_command(edit::merge_junctions(
              deps.document.network(), selected_junctions[0], selected_junctions[1]));
        }});
    items.push_back(separator());
    // Maneuvers (p4-s6, issue #227). Rebuild replans every turn from the arms,
    // discarding hand-shaped geometry and per-turn locks; it is invalid unless
    // something geometric is authored, and impossible without arms to replan
    // from, so both conditions gate the item rather than letting it no-op.
    const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction);
    const bool rebuildable =
        arm_based && std::ranges::any_of(maneuvers, [](const JunctionManeuverInfo& info) {
          return info.locked || !info.control_points.empty() || info.start_offset != 0.0 ||
                 info.end_offset != 0.0;
        });
    items.push_back(MenuItem{.text = QObject::tr("Rebuild maneuvers"),
                             .enabled = rebuildable,
                             .invoke = [deps, junction] {
                               (void)deps.document.push_command(
                                   edit::rebuild_maneuvers(deps.document.network(), junction));
                             }});
    // The one turn the planner never emits: a U-turn back onto the arm it came
    // from. One child per arm — a fit can legitimately be refused (two adjacent
    // lanes are a tight hairpin), and the kernel stays the arbiter.
    if (arm_based) {
      MenuItem uturns{.text = QObject::tr("Add U-Turn…")};
      for (const RoadEnd& arm : junction_ptr->arms) {
        const Road* arm_road = network.road(arm.road);
        if (arm_road == nullptr) {
          continue;
        }
        uturns.children.push_back(
            MenuItem{.text = QObject::tr("On arm %1").arg(QString::fromStdString(arm_road->odr_id)),
                     .invoke = [deps, junction, arm] {
                       (void)deps.document.push_command(
                           edit::add_uturn_maneuver(deps.document.network(), junction, arm));
                     }});
      }
      uturns.enabled = !uturns.children.empty();
      items.push_back(std::move(uturns));
    }

    // Signalization (p4-s7, issue #228). Gated on exactly what
    // signalize_junction validates, so an item never fails when clicked: arms
    // to place against (a foreign junction has none), no spans (ASAM: a virtual
    // junction "shall not have controllers"), a solvable approach, and — per
    // template — a plan that is not already applied, since re-applying the same
    // one is a no-op the command layer refuses.
    const bool signalizable = arm_based && !junction_signals(network, junction).empty();
    // Display names, in edit::SignalizeTemplate's own order — the same order
    // kSignalizationTemplates uses, which is what makes the index the mapping.
    const std::array<QString, 4> template_names{QObject::tr("Protected left (4-phase)"),
                                                QObject::tr("Two phase (permissive lefts)"),
                                                QObject::tr("All-way stop"),
                                                QObject::tr("Two-way stop")};
    MenuItem signalize{.text = QObject::tr("Auto signalize")};
    for (std::size_t i = 0; i < std::size(kSignalizationTemplates); ++i) {
      const std::string_view token = kSignalizationTemplates[i];
      const auto tmpl = static_cast<edit::SignalizeTemplate>(i);
      // The context menu never mounts a prop (the Attributes pane's combo does),
      // so "already applied" means this template with no mount.
      const bool applied = junction_ptr != nullptr && junction_ptr->signalization.tmpl == token &&
                           junction_ptr->signalization.mount_model.empty();
      signalize.children.push_back(
          MenuItem{.text = template_names[i],
                   .enabled = signalizable && !applied,
                   .invoke = [deps, junction, tmpl] {
                     (void)deps.document.push_command(edit::signalize_junction(
                         deps.document.network(), junction, edit::SignalizeOptions{.tmpl = tmpl}));
                   }});
    }
    signalize.enabled = signalizable;
    items.push_back(std::move(signalize));
    const bool clearable =
        junction_ptr != nullptr &&
        (!junction_ptr->junction_controllers.empty() || !junction_ptr->signal_mounts.empty() ||
         !junction_ptr->signalization.tmpl.empty());
    items.push_back(MenuItem{.text = QObject::tr("Clear signalization"),
                             .enabled = clearable,
                             .invoke = [deps, junction] {
                               (void)deps.document.push_command(
                                   edit::clear_signalization(deps.document.network(), junction));
                             }});

    // Signal Phases… (p4-s8, issue #229): opens the timeline editor for a
    // light-controlled junction. Enabled only when the junction is dynamic
    // (carries controllers) — a static all-way/two-way stop has no cycle. The
    // callback is MainWindow's (opening a dock is not a document command); a
    // headless test without it gets a disabled item.
    bool dynamic = false;
    for (const JunctionApproachInfo& approach : junction_signals(network, junction)) {
      if (approach.dynamic || !approach.controller_odr_ids.empty()) {
        dynamic = true;
        break;
      }
    }
    if (deps.open_signal_phase_editor) {
      items.push_back(MenuItem{
          .text = QObject::tr("Signal Phases…"), .enabled = dynamic, .invoke = [deps, junction] {
            deps.selection.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
            deps.open_signal_phase_editor(junction);
          }});
    }
    items.push_back(separator());
    // Author one zebra crosswalk per arm, spanning its driving lanes just inside
    // the junction — all in one undo step (§WS-B). Disabled when the junction has
    // no resolvable arms (foreign/degenerate) so it can't no-op silently.
    const bool has_arms = !edit::junction_crosswalks(network, junction).empty();
    items.push_back(MenuItem{
        .text = QObject::tr("Add crosswalks to all arms"),
        .enabled = has_arms,
        .invoke = [deps, junction] {
          // Born linked to the default crosswalk asset (p3-s2), so a
          // later asset edit re-materializes these instances.
          const edit::CrosswalkParams params = deps.default_crosswalk_params
                                                   ? deps.default_crosswalk_params()
                                                   : edit::CrosswalkParams{};
          auto crosswalks = edit::junction_crosswalks(deps.document.network(), junction, params);
          if (crosswalks.empty()) {
            return;
          }
          deps.document.undo_stack()->beginMacro(QObject::tr("Add crosswalks"));
          for (auto& [road, object] : crosswalks) {
            // Link each crosswalk to its arm's (already derived) stop line
            // inside the same macro, so the setback and the provenance match
            // what the interactive tool records.
            const std::string crosswalk_odr = object.odr_id;
            const RoadId arm_road = road;
            (void)deps.document.push_command(
                edit::add_object(deps.document.network(), road, std::move(object)));
            for (const ContactPoint contact : {ContactPoint::Start, ContactPoint::End}) {
              const RoadEnd arm{.road = arm_road, .contact = contact};
              if (edit::junction_at_end(deps.document.network(), arm) != junction) {
                continue;
              }
              (void)deps.document.push_command(edit::set_stopline_distance(
                  deps.document.network(), junction, arm, kStopLineDefaultDistance, crosswalk_odr));
            }
          }
          deps.document.undo_stack()->endMacro();
        }});
    // NOTE: there is no "Add stop lines to all arms" action any more (p4-s3,
    // #318). Every arm already HAS a stop line — they are derived, meshed and
    // exported without anything being authored — so the action would be a no-op.
    // Editing one is the StopLine tool's job.
    // A straight lane arrow on each approach lane, pointing into the junction.
    const bool has_arrow_arms = !edit::junction_lane_arrows(network, junction).empty();
    items.push_back(
        MenuItem{.text = QObject::tr("Add lane arrows to all arms"),
                 .enabled = has_arrow_arms,
                 .invoke = [deps, junction] {
                   auto arrows = edit::junction_lane_arrows(deps.document.network(), junction);
                   if (arrows.empty()) {
                     return;
                   }
                   deps.document.undo_stack()->beginMacro(QObject::tr("Add lane arrows"));
                   for (auto& [road, object] : arrows) {
                     (void)deps.document.push_command(
                         edit::add_object(deps.document.network(), road, std::move(object)));
                   }
                   deps.document.undo_stack()->endMacro();
                 }});
    // A double-yellow centre line down every arm. Unlike the three above these
    // are lane roadMarks, not objects, so they go through set_road_mark and
    // replace the single centre line the road profile laid down.
    const bool has_center_arms = !edit::junction_center_marks(network, junction).empty();
    items.push_back(
        MenuItem{.text = QObject::tr("Add centre lines to all arms"),
                 .enabled = has_center_arms,
                 .invoke = [deps, junction] {
                   auto marks = edit::junction_center_marks(deps.document.network(), junction);
                   if (marks.empty()) {
                     return;
                   }
                   deps.document.undo_stack()->beginMacro(QObject::tr("Add centre lines"));
                   for (auto& [lane, mark] : marks) {
                     (void)deps.document.push_command(
                         edit::set_road_mark(deps.document.network(), lane, std::move(mark)));
                   }
                   deps.document.undo_stack()->endMacro();
                 }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Delete junction"), .invoke = [deps, junction] {
                               (void)deps.document.push_command(
                                   edit::delete_junction(deps.document.network(), junction));
                             }});
    return items;
  }

  // Object/prop menu — a placed prop sits on the road surface, so it wins over
  // the road body (like a node does).
  if (context.pick.has_value() && context.pick->object.is_valid()) {
    const ObjectId object = context.pick->object;
    const RoadId road = context.pick->road;
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps, road, object] {
                               select_object(deps, road, object);
                               deps.actions.frame_selection->trigger();
                             }});
    // Arm the Move tool with this prop selected (discoverable move path, #176).
    items.push_back(MenuItem{.text = QObject::tr("Move"), .invoke = [deps, road, object] {
                               select_object(deps, road, object);
                               deps.actions.tool_move->trigger();
                             }});
    // Duplicate the prop a few meters further along its road (same model/pose).
    const Object* source = network.object(object);
    const Road* owner = source != nullptr ? network.road(source->road) : nullptr;
    items.push_back(MenuItem{
        .text = QObject::tr("Duplicate"), .enabled = owner != nullptr, .invoke = [deps, object] {
          const Object* src = deps.document.network().object(object);
          const Road* road_ptr = src != nullptr ? deps.document.network().road(src->road) : nullptr;
          if (road_ptr == nullptr) {
            return;
          }
          Object copy = *src;
          copy.odr_id = next_object_odr_id(deps.document.network());
          const double length = road_ptr->plan_view.length();
          copy.s = src->s + 5.0 <= length ? src->s + 5.0 : std::max(0.0, src->s - 5.0);
          (void)deps.document.push_command(
              edit::add_object(deps.document.network(), src->road, std::move(copy)));
        }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Delete object"), .invoke = [deps, object] {
                               (void)deps.document.push_command(
                                   edit::delete_object(deps.document.network(), object));
                             }});
    return items;
  }

  // Road-body menu.
  if (context.pick.has_value()) {
    const RoadId road = context.pick->road;
    const std::optional<double> station = context.station;
    // Lane removal — when a specific lane was picked, offer to remove it (the
    // road items stay reachable below). Enabled per the kernel's outermost /
    // non-center rule; disabled (greyed) otherwise (gate finding 6).
    if (context.pick->lane.is_valid()) {
      const LaneId lane = context.pick->lane;
      items.push_back(MenuItem{.text = QObject::tr("Remove this lane"),
                               .enabled = lane_removable(network, lane),
                               .invoke = [deps, lane] {
                                 (void)deps.document.push_command(
                                     edit::remove_lane(deps.document.network(), lane));
                               }});
      items.push_back(separator());
    }
    // Arm the Move tool with this road selected (discoverable move path, #176).
    items.push_back(MenuItem{.text = QObject::tr("Move"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.tool_move->trigger();
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Insert bend point here"),
                             .enabled = station.has_value(),
                             .invoke = [deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::insert_node_at(deps.document.network(), road, *station));
                               }
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Split road here"),
                             .enabled = station.has_value(),
                             .invoke = [deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::split_road(deps.document.network(), road, *station));
                               }
                             }});
    // Merge selected roads — enabled only for exactly two mergeable roads.
    const std::vector<RoadId> selected = deps.selection.selected_roads();
    const bool mergeable = selected.size() == 2 &&
                           (edit::check_mergeable(network, selected[0], selected[1]).has_value() ||
                            edit::check_mergeable(network, selected[1], selected[0]).has_value());
    items.push_back(MenuItem{
        .text = QObject::tr("Merge selected roads"),
        .enabled = mergeable,
        .invoke = [deps, selected] {
          if (selected.size() != 2) {
            return;
          }
          RoadId a = selected[0];
          RoadId b = selected[1];
          if (!edit::check_mergeable(deps.document.network(), a, b).has_value()) {
            std::swap(a, b);
          }
          (void)deps.document.push_command(edit::merge_roads(deps.document.network(), a, b));
        }});
    // Link Ends — weld two selected roads' nearby free ends (G2, finding 3).
    const std::optional<std::pair<RoadEnd, RoadEnd>> link_pair =
        selected.size() == 2 ? linkable_ends(network, selected[0], selected[1]) : std::nullopt;
    items.push_back(MenuItem{.text = QObject::tr("Link Ends"),
                             .enabled = link_pair.has_value(),
                             .invoke = [deps, link_pair] {
                               if (!link_pair.has_value()) {
                                 return;
                               }
                               (void)deps.document.push_command(edit::close_gap(
                                   deps.document.network(), link_pair->first, link_pair->second));
                             }});
    // Connecting-road (maneuver) verbs, p4-s6 issue #227. A road that belongs
    // to a junction IS one of its turns, so the two per-maneuver commands that
    // need no drag are offered right where the turn was clicked.
    if (const Road* road_ptr = network.road(road);
        road_ptr != nullptr && road_ptr->junction.is_valid()) {
      const JunctionId junction = road_ptr->junction;
      const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, junction);
      const auto match = std::ranges::find_if(
          maneuvers, [road](const JunctionManeuverInfo& info) { return info.road == road; });
      if (match != maneuvers.end()) {
        items.push_back(separator());
        // "Convert to Explicit" is the lock: an explicit turn keeps its plan
        // view, length, elevation and lane widths through a re-derive, and is
        // kept even when the plan no longer contains it.
        items.push_back(MenuItem{
            .text = match->locked ? QObject::tr("Return maneuver to derived (unlock geometry)")
                                  : QObject::tr("Convert to explicit (lock geometry)"),
            .invoke = [deps, junction, road, locked = match->locked] {
              (void)deps.document.push_command(
                  edit::set_maneuver_locked(deps.document.network(), junction, road, !locked));
            }});
        const Junction* junction_ptr = network.junction(junction);
        const bool replannable = junction_ptr != nullptr && !junction_ptr->arms.empty() &&
                                 junction_ptr->spans.empty() && match->authored &&
                                 !match->is_uturn_explicit;
        items.push_back(MenuItem{.text = QObject::tr("Reset maneuver"),
                                 .enabled = replannable,
                                 .invoke = [deps, junction, road] {
                                   (void)deps.document.push_command(edit::reset_maneuver(
                                       deps.document.network(), junction, road));
                                 }});
      }
    }
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Edit lane profile"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.tool_lane_profile->trigger();
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Edit elevation profile"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.tool_elevation->trigger();
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.frame_selection->trigger();
                             }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Delete road"), .invoke = [deps, road] {
                               (void)deps.document.push_command(
                                   edit::delete_road(deps.document.network(), road));
                             }});
    return items;
  }

  // Empty context: scene-wide.
  items.push_back(MenuItem{.text = QObject::tr("Create road here"),
                           .invoke = [deps] { deps.actions.tool_create_road->trigger(); }});
  items.push_back(MenuItem{.text = QObject::tr("Add from library…"),
                           .invoke = [deps] { deps.actions.add_from_library->trigger(); }});
  items.push_back(MenuItem{.text = QObject::tr("Paste"), .enabled = false}); // stub (no clipboard)
  items.push_back(separator());
  items.push_back(MenuItem{.text = QObject::tr("Frame all"), .invoke = [deps] {
                             deps.selection.clear();
                             deps.actions.frame_selection->trigger();
                           }});
  return items;
}

QMenu* assemble_context_menu(const MenuContext& context, ContextMenuDeps& deps, QWidget* parent) {
  const std::vector<MenuItem> items = build_context_menu(context, deps);
  if (items.empty()) {
    return nullptr;
  }
  auto* menu = new QMenu(parent);
  fill_menu(*menu, items);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  return menu;
}

} // namespace roadmaker::editor
