#include "app/context_menu.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QUndoStack>
#include <algorithm>
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

} // namespace

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
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [deps, road] {
                               select_road(deps, road);
                               deps.actions.frame_selection->trigger();
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
    // Author one zebra crosswalk per arm, spanning its driving lanes just inside
    // the junction — all in one undo step (§WS-B). Disabled when the junction has
    // no resolvable arms (foreign/degenerate) so it can't no-op silently.
    const bool has_arms = !edit::junction_crosswalks(network, junction).empty();
    items.push_back(
        MenuItem{.text = QObject::tr("Add crosswalks to all arms"),
                 .enabled = has_arms,
                 .invoke = [deps, junction] {
                   auto crosswalks = edit::junction_crosswalks(deps.document.network(), junction);
                   if (crosswalks.empty()) {
                     return;
                   }
                   deps.document.undo_stack()->beginMacro(QObject::tr("Add crosswalks"));
                   for (auto& [road, object] : crosswalks) {
                     (void)deps.document.push_command(
                         edit::add_object(deps.document.network(), road, std::move(object)));
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
  for (const MenuItem& item : items) {
    if (item.separator) {
      menu->addSeparator();
      continue;
    }
    QAction* action = menu->addAction(item.text);
    action->setEnabled(item.enabled);
    if (item.invoke) {
      QObject::connect(action, &QAction::triggered, menu, [invoke = item.invoke] { invoke(); });
    }
  }
  menu->setAttribute(Qt::WA_DeleteOnClose);
  return menu;
}

} // namespace roadmaker::editor
