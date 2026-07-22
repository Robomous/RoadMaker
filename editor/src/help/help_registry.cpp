#include "help/help_registry.hpp"

#include <array>

namespace roadmaker::editor::help {
namespace {

// Every ToolId, mapped to the guide page that documents it. Tools with no page
// of their own point at the page that covers their gesture: Select and Move
// both live on Moving and transforming; Delete's behaviour is documented with
// the right-click menus. Keep this exhaustive — the coverage test loops the
// whole ToolId enum and fails the build on a hole.
constexpr std::array<ToolPage, 26> kToolPages{{
    {ToolId::Select, "moving-and-transforming"},
    {ToolId::Move, "moving-and-transforming"},
    {ToolId::CreateRoad, "create-road"},
    {ToolId::EditNodes, "edit-nodes"},
    {ToolId::LaneProfile, "lane-profile"},
    {ToolId::Elevation, "elevation"},
    {ToolId::CreateJunction, "junction"},
    {ToolId::Split, "merge-split"},
    {ToolId::Delete, "context-menus"},
    {ToolId::LaneAdd, "lane-add"},
    {ToolId::LaneForm, "lane-form"},
    {ToolId::LaneCarve, "lane-carve"},
    {ToolId::Crosswalk, "junction"}, // crosswalks are placed on junction approaches
    {ToolId::MarkingPoint, "markings"},
    {ToolId::MarkingCurve, "markings"},
    {ToolId::PropPoint, "objects-signals"}, // props are objects & signals content
    {ToolId::PropCurve, "objects-signals"},
    {ToolId::PropSpan, "objects-signals"},
    {ToolId::PropPolygon, "objects-signals"},
    {ToolId::Corner, "junction"}, // corners are a junction's fillets
    // The junction-authoring tools all document their gestures on the junction
    // page. These four shipped WITHOUT a row: the coverage loop below stopped at
    // Corner and its comment wrongly called Corner the last enumerator, so the
    // gate never noticed (p4-s7, issue #228).
    {ToolId::StopLine, "junction"},
    {ToolId::JunctionSpan, "junction"},
    {ToolId::JunctionSurface, "junction"},
    {ToolId::Maneuver, "junction"},
    {ToolId::Signal, "junction"},      // signalization is authored on a junction
    {ToolId::Sign, "objects-signals"}, // road signs are placed signal entities
}};

// Every dockable panel, keyed by the QDockWidget objectName set in
// main_window.cpp (search there for setObjectName("dock.*")). The 2D editor
// dock hosts the Lane Width editor, so it maps to that page.
constexpr std::array<DockPage, 5> kDockPages{{
    {"dock.scene", "scene-tree"},
    {"dock.library", "library"},
    {"dock.properties", "attributes"},
    {"dock.editor2d", "lane-width"},
    {"dock.diagnostics", "diagnostics"},
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
