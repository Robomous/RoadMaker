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

#include "help/help_registry.hpp"

#include <array>

namespace roadmaker::editor::help {
namespace {

// Every ToolId, mapped to the guide page that documents it. Tools with no page
// of their own point at the page that covers their gesture: Select and Move
// both live on Moving and transforming; Delete's behaviour is documented with
// the right-click menus. Keep this exhaustive — the coverage test loops the
// whole ToolId enum and fails the build on a hole.
constexpr std::array<ToolPage, 28> kToolPages{{
    {ToolId::Select, "reference/moving-and-transforming"},
    {ToolId::Move, "reference/moving-and-transforming"},
    {ToolId::CreateRoad, "reference/create-road"},
    {ToolId::EditNodes, "reference/edit-nodes"},
    {ToolId::LaneProfile, "reference/lane-profile"},
    {ToolId::Elevation, "reference/elevation"},
    {ToolId::CreateJunction, "reference/junction"},
    {ToolId::Split, "reference/merge-split"},
    {ToolId::Delete, "reference/context-menus"},
    {ToolId::LaneAdd, "reference/lane-add"},
    {ToolId::LaneForm, "reference/lane-form"},
    {ToolId::LaneCarve, "reference/lane-carve"},
    {ToolId::Crosswalk, "reference/junction"}, // crosswalks are placed on junction approaches
    {ToolId::MarkingPoint, "reference/markings"},
    {ToolId::MarkingCurve, "reference/markings"},
    {ToolId::PropPoint, "reference/objects-signals"}, // props are objects & signals content
    {ToolId::PropCurve, "reference/objects-signals"},
    {ToolId::PropSpan, "reference/objects-signals"},
    {ToolId::PropPolygon, "reference/objects-signals"},
    {ToolId::Corner, "reference/junction"}, // corners are a junction's fillets
    // The junction-authoring tools all document their gestures on the junction
    // page. These four shipped WITHOUT a row: the coverage loop below stopped at
    // Corner and its comment wrongly called Corner the last enumerator, so the
    // gate never noticed (p4-s7, issue #228).
    {ToolId::StopLine, "reference/junction"},
    {ToolId::JunctionSpan, "reference/junction"},
    {ToolId::JunctionSurface, "reference/junction"},
    {ToolId::Maneuver, "reference/junction"},
    {ToolId::Signal, "reference/junction"},            // signalization is authored on a junction
    {ToolId::Sign, "reference/objects-signals"},       // road signs are placed signal entities
    {ToolId::Surface, "reference/ground-surfaces"},    // P5 terrain: the ground surface itself
    {ToolId::TerrainBrush, "reference/terrain-brush"}, // P5 terrain: sculpting the height field
}};

// Every dockable panel, keyed by the QDockWidget objectName set in
// main_window.cpp (search there for setObjectName("dock.*")). The 2D editor
// dock hosts the Lane Width editor, so it maps to that page.
constexpr std::array<DockPage, 5> kDockPages{{
    {"dock.scene", "reference/scene-tree"},
    {"dock.library", "reference/library"},
    {"dock.properties", "reference/attributes"},
    {"dock.editor2d", "reference/lane-width"},
    {"dock.diagnostics", "reference/diagnostics"},
}};

} // namespace

std::span<const ToolPage> tool_table() {
  return kToolPages;
}

std::span<const DockPage> dock_table() {
  return kDockPages;
}

QString page_for_tool(ToolId id) {
  for (const ToolPage& row : kToolPages) {
    if (row.id == id) {
      return QString::fromUtf8(row.slug);
    }
  }
  return {};
}

QString page_for_dock(QStringView dock_name) {
  if (dock_name.isEmpty()) {
    return {};
  }
  for (const DockPage& row : kDockPages) {
    if (dock_name == QString::fromUtf8(row.dock)) {
      return QString::fromUtf8(row.slug);
    }
  }
  return {};
}

QString context_page(std::optional<ToolId> active_tool, QStringView focused_dock) {
  // Focused dock wins: if the user tabbed into a panel, F1 is about that panel,
  // not whatever tool happens to be armed in the viewport.
  const QString dock_page = page_for_dock(focused_dock);
  if (!dock_page.isEmpty()) {
    return dock_page;
  }
  if (active_tool.has_value()) {
    const QString tool_page = page_for_tool(*active_tool);
    if (!tool_page.isEmpty()) {
      return tool_page;
    }
  }
  return QStringLiteral("index");
}

} // namespace roadmaker::editor::help
