#include "app/context_menu.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QAction>
#include <QMenu>
#include <QObject>

#include "app/actions.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

void select_road(ContextMenuDeps& deps, RoadId road) {
  deps.selection.select({.road = road, .lane = LaneId{}}, SelectMode::Replace);
}

MenuItem separator() {
  return MenuItem{.separator = true};
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
                             .invoke = [&deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::split_road(deps.document.network(), road, *station));
                               }
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Delete node"), .invoke = [&deps, road, index] {
                               (void)deps.document.push_command(
                                   edit::delete_waypoint(deps.document.network(), road, index));
                             }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [&deps, road] {
                               select_road(deps, road);
                               deps.actions.frame_selection->trigger();
                             }});
    return items;
  }

  // Road-body menu.
  if (context.pick.has_value()) {
    const RoadId road = context.pick->road;
    const std::optional<double> station = context.station;
    items.push_back(MenuItem{.text = QObject::tr("Insert bend point here"),
                             .enabled = station.has_value(),
                             .invoke = [&deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::insert_node_at(deps.document.network(), road, *station));
                               }
                             }});
    items.push_back(MenuItem{.text = QObject::tr("Split road here"),
                             .enabled = station.has_value(),
                             .invoke = [&deps, road, station] {
                               if (station.has_value()) {
                                 (void)deps.document.push_command(
                                     edit::split_road(deps.document.network(), road, *station));
                               }
                             }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Edit lane profile"), .invoke = [&deps, road] {
                               select_road(deps, road);
                               deps.actions.tool_lane_profile->trigger();
                             }});
    items.push_back(
        MenuItem{.text = QObject::tr("Edit elevation profile"), .invoke = [&deps, road] {
                   select_road(deps, road);
                   deps.actions.tool_elevation->trigger();
                 }});
    items.push_back(MenuItem{.text = QObject::tr("Frame"), .invoke = [&deps, road] {
                               select_road(deps, road);
                               deps.actions.frame_selection->trigger();
                             }});
    items.push_back(separator());
    items.push_back(MenuItem{.text = QObject::tr("Delete road"), .invoke = [&deps, road] {
                               (void)deps.document.push_command(
                                   edit::delete_road(deps.document.network(), road));
                             }});
    return items;
  }

  // Empty context: scene-wide.
  items.push_back(MenuItem{.text = QObject::tr("Frame all"), .invoke = [&deps] {
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
