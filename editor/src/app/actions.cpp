#include "app/actions.hpp"

#include "app/icons.hpp"
#include "app/shortcut_registry.hpp"

namespace roadmaker::editor {

Actions::Actions(QUndoStack& undo_stack, QObject* parent) : QObject(parent) {
  // iconText carries the short label the labeled toolbar renders under the
  // icon (ToolButtonTextUnderIcon); the menu keeps the full text.
  new_file = new QAction(tr("&New"), this);
  new_file->setShortcuts(shortcuts::sequences(shortcuts::Id::NewScene));
  new_file->setIconText(tr("New"));
  new_file->setToolTip(tr("New scene — start an empty road network"));

  open = new QAction(tr("&Open…"), this);
  open->setShortcuts(shortcuts::sequences(shortcuts::Id::Open));
  open->setIconText(tr("Open"));
  open->setToolTip(tr("Open an OpenDRIVE (.xodr) file"));

  save = new QAction(tr("&Save"), this);
  save->setShortcuts(shortcuts::sequences(shortcuts::Id::Save));
  save->setIconText(tr("Save"));
  save->setToolTip(tr("Save the scene as OpenDRIVE"));

  save_as = new QAction(tr("Save &As…"), this);
  save_as->setShortcuts(shortcuts::sequences(shortcuts::Id::SaveAs));

  export_glb = new QAction(tr("&Export glTF…"), this);
  export_glb->setEnabled(false); // enabled once a file is loaded
  export_glb->setIconText(tr("Export"));
  export_glb->setToolTip(tr("Export the network as binary glTF (.glb)"));

#ifdef RM_HAVE_USD
  export_usd = new QAction(tr("Export &USD…"), this);
  export_usd->setEnabled(false); // enabled once a file is loaded
#endif

  quit = new QAction(tr("&Quit"), this);
  quit->setShortcuts(shortcuts::sequences(shortcuts::Id::Quit));
  quit->setMenuRole(QAction::QuitRole);

  undo = undo_stack.createUndoAction(this, tr("&Undo"));
  undo->setShortcuts(shortcuts::sequences(shortcuts::Id::Undo));
  redo = undo_stack.createRedoAction(this, tr("&Redo"));
  redo->setShortcuts(shortcuts::sequences(shortcuts::Id::Redo));

  tool_group = new QActionGroup(this);
  tool_select = new QAction(tr("&Select/Move"), this);
  tool_select->setCheckable(true);
  tool_select->setChecked(true); // the default tool
  // Q, not V: V is frame-on-cursor (GW-1 step 10). Rebound in p1-s2.
  tool_select->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolSelect));
  tool_select->setIconText(tr("Select"));
  tool_select->setToolTip(tr("Select/Move — click picks, drag spans a rubber band, "
                             "drag a node handle moves it (Q)"));
  tool_group->addAction(tool_select);

  tool_move = new QAction(tr("&Move"), this);
  tool_move->setCheckable(true);
  tool_move->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolMove));
  tool_move->setIconText(tr("Move"));
  tool_move->setToolTip(tr("Move — hover shows the 4-arrow cursor; drag a road or a prop to "
                           "move it, or click to select and transform it (M)"));
  tool_group->addAction(tool_move);

  tool_create_road = new QAction(tr("&Create Road"), this);
  tool_create_road->setCheckable(true);
  tool_create_road->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolCreateRoad));
  tool_create_road->setIconText(tr("Road"));
  tool_create_road->setToolTip(tr("Create Road — click places waypoints, Enter or double-click "
                                  "creates the road, Esc cancels (C)"));
  tool_group->addAction(tool_create_road);

  tool_edit_nodes = new QAction(tr("Edit &Nodes"), this);
  tool_edit_nodes->setCheckable(true);
  tool_edit_nodes->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolEditNodes));
  tool_edit_nodes->setIconText(tr("Nodes"));
  tool_edit_nodes->setToolTip(tr("Edit Nodes — drag a node to move it, click a midpoint "
                                 "marker to insert, Delete removes the active node (N)"));
  tool_group->addAction(tool_edit_nodes);

  tool_lane_profile = new QAction(tr("&Lane"), this);
  tool_lane_profile->setCheckable(true);
  tool_lane_profile->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolLaneProfile));
  tool_lane_profile->setIconText(tr("Lanes"));
  tool_lane_profile->setToolTip(tr("Lane — click a lane to select it; Delete removes it. Edit "
                                   "type in Properties, width along s in the 2D Editor (L)"));
  tool_group->addAction(tool_lane_profile);

  tool_elevation = new QAction(tr("&Elevation"), this);
  tool_elevation->setCheckable(true);
  tool_elevation->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolElevation));
  tool_elevation->setIconText(tr("Elevation"));
  tool_elevation->setToolTip(tr("Elevation — click a road node, then set its height in the "
                                "Properties panel; the grade re-fits smoothly (E)"));
  tool_group->addAction(tool_elevation);

  tool_create_junction = new QAction(tr("Create &Junction"), this);
  tool_create_junction->setCheckable(true);
  tool_create_junction->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolCreateJunction));
  tool_create_junction->setIconText(tr("Junction"));
  tool_create_junction->setToolTip(tr("Create Junction — click 2+ road ends, or 1 end + a road "
                                      "body to tee into it; Enter generates, Esc cancels (J)"));
  tool_group->addAction(tool_create_junction);

  tool_split = new QAction(tr("&Split"), this);
  tool_split->setCheckable(true);
  tool_split->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolSplit));
  tool_split->setIconText(tr("Split"));
  tool_split->setToolTip(tr("Split — click a road to cut it in two at the marker (K)"));
  tool_group->addAction(tool_split);

  tool_delete = new QAction(tr("&Delete"), this);
  tool_delete->setCheckable(true);
  tool_delete->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolDelete));
  tool_delete->setIconText(tr("Delete"));
  tool_delete->setToolTip(tr("Delete — click a road to delete it, undo restores (X)"));
  tool_group->addAction(tool_delete);

  tool_lane_add = new QAction(tr("Lane &Add"), this);
  tool_lane_add->setCheckable(true);
  tool_lane_add->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolLaneAdd));
  tool_lane_add->setIconText(tr("Lane Add"));
  tool_lane_add->setToolTip(
      tr("Lane Add — drag along a road to add a lane pocket over that span (A)"));
  tool_group->addAction(tool_lane_add);

  tool_lane_form = new QAction(tr("Lane &Form"), this);
  tool_lane_form->setCheckable(true);
  tool_lane_form->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolLaneForm));
  tool_lane_form->setIconText(tr("Lane Form"));
  tool_lane_form->setToolTip(
      tr("Lane Form — click a road to form a lane from there to its end (⇧A)"));
  tool_group->addAction(tool_lane_form);

  // Not a tool — a command that surfaces the 2D Editor's Lane Width tab for the
  // selected lane. Standalone so ⇧L works whatever tool is active.
  lane_width_editor = new QAction(tr("Lane &Width Editor"), this);
  lane_width_editor->setShortcuts(shortcuts::sequences(shortcuts::Id::LaneWidthEditor));
  lane_width_editor->setIconText(tr("Width"));
  lane_width_editor->setToolTip(tr("Open the 2D Editor's Lane Width tab for the selected "
                                   "lane — drag the width curve along s (⇧L)"));

  // Road templates for the Create Road tool (02_editing_tools.md §2):
  // exclusive and always checked; the toolbar shows them as a dropdown.
  template_group = new QActionGroup(this);
  template_rural = new QAction(tr("Two-lane &Rural"), this);
  template_rural->setCheckable(true);
  template_rural->setChecked(true); // the tool's default profile
  template_rural->setToolTip(tr("One driving lane each way, right-hand shoulder"));
  template_group->addAction(template_rural);

  template_urban = new QAction(tr("&Urban Sidewalk"), this);
  template_urban->setCheckable(true);
  template_urban->setToolTip(tr("One driving lane each way, sidewalks both sides"));
  template_group->addAction(template_urban);

  template_highway = new QAction(tr("&Highway"), this);
  template_highway->setCheckable(true);
  template_highway->setToolTip(tr("Two driving lanes each way, wide shoulders"));
  template_group->addAction(template_highway);

  reset_camera = new QAction(tr("Reset &Camera"), this);
  reset_camera->setIconText(tr("Camera"));
  reset_camera->setToolTip(tr("Reset the camera to the default view"));
  add_from_library = new QAction(tr("Add from &Library…"), this);
  add_from_library->setIconText(tr("Library"));
  add_from_library->setToolTip(tr("Open the Library panel to drag in roads, intersections, "
                                  "and props"));
  frame_selection = new QAction(tr("&Frame Selection"), this);
  frame_selection->setShortcuts(shortcuts::sequences(shortcuts::Id::FrameSelection));
  frame_selection->setIconText(tr("Frame"));
  frame_selection->setToolTip(tr("Frame the selection — the whole scene when "
                                 "nothing is selected (F)"));
  frame_cursor = new QAction(tr("Frame Under &Cursor"), this);
  frame_cursor->setShortcuts(shortcuts::sequences(shortcuts::Id::FrameCursor));
  frame_cursor->setIconText(tr("Frame Cursor"));
  frame_cursor->setToolTip(tr("Move the pivot to the point under the cursor, keeping the "
                              "zoom (V)"));

  // Projection (GW-1 step 11). Exclusive: the view is one or the other.
  projection_group = new QActionGroup(this);
  view_perspective = new QAction(tr("&Perspective"), this);
  view_perspective->setShortcuts(shortcuts::sequences(shortcuts::Id::ViewPerspective));
  view_perspective->setCheckable(true);
  view_perspective->setChecked(true); // the startup projection
  view_perspective->setToolTip(tr("Perspective projection (P)"));
  view_orthographic = new QAction(tr("&Orthographic"), this);
  view_orthographic->setShortcuts(shortcuts::sequences(shortcuts::Id::ViewOrthographic));
  view_orthographic->setCheckable(true);
  view_orthographic->setToolTip(tr("Orthographic projection — parallel, no foreshortening (O)"));
  projection_group->addAction(view_perspective);
  projection_group->addAction(view_orthographic);

  // Cardinal views (GW-1 steps 12-13). The numpad digits are the primary
  // binding; the top-row digits are the alternate for keyboards without one.
  const auto make_cardinal = [this](const QString& text, const QString& tip, shortcuts::Id id) {
    auto* action = new QAction(text, this);
    action->setShortcuts(shortcuts::sequences(id)); // numpad + the top-row alternate
    action->setToolTip(tip);
    return action;
  };
  view_north = make_cardinal(tr("Look from &North"),
                             tr("Look from the north, southward (numpad 8, or 8)"),
                             shortcuts::Id::ViewNorth);
  view_south = make_cardinal(tr("Look from &South"),
                             tr("Look from the south, northward (numpad 2, or 2)"),
                             shortcuts::Id::ViewSouth);
  view_west = make_cardinal(tr("Look from &West"),
                            tr("Look from the west, eastward (numpad 4, or 4)"),
                            shortcuts::Id::ViewWest);
  view_east = make_cardinal(tr("Look from &East"),
                            tr("Look from the east, westward (numpad 6, or 6)"),
                            shortcuts::Id::ViewEast);
  view_top = make_cardinal(tr("&Top-Down"),
                           tr("Plan view from directly above, north up (numpad 5, or 5)"),
                           shortcuts::Id::ViewTop);
  merge_roads = new QAction(tr("&Merge Roads"), this);
  merge_roads->setIconText(tr("Merge"));
  merge_roads->setEnabled(false); // enabled only for a mergeable 2-road selection
  merge_roads->setToolTip(tr("Merge — join two selected roads that meet end-to-start"));

  reset_layout = new QAction(tr("Reset &Layout"), this);

  about = new QAction(tr("&About RoadMaker"), this);
  about->setMenuRole(QAction::AboutRole);

  apply_icons();
}

void Actions::apply_icons() {
  // Bundled palette-tinted set (docs/design/m2/05_assets.md §1) — never
  // QIcon::fromTheme, which is empty on macOS/Windows.
  new_file->setIcon(Icons::get(QStringLiteral("file-plus")));
  open->setIcon(Icons::get(QStringLiteral("folder-open")));
  save->setIcon(Icons::get(QStringLiteral("save")));
  save_as->setIcon(Icons::get(QStringLiteral("save")));
  export_glb->setIcon(Icons::get(QStringLiteral("box")));
#ifdef RM_HAVE_USD
  export_usd->setIcon(Icons::get(QStringLiteral("box")));
#endif
  undo->setIcon(Icons::get(QStringLiteral("undo-2")));
  redo->setIcon(Icons::get(QStringLiteral("redo-2")));
  tool_select->setIcon(Icons::get(QStringLiteral("mouse-pointer-2")));
  tool_move->setIcon(Icons::get(QStringLiteral("move")));
  tool_create_road->setIcon(Icons::get(QStringLiteral("clothoid-road")));
  tool_edit_nodes->setIcon(Icons::get(QStringLiteral("waypoints")));
  tool_lane_profile->setIcon(Icons::get(QStringLiteral("lane-section")));
  lane_width_editor->setIcon(Icons::get(QStringLiteral("lane-section")));
  tool_elevation->setIcon(Icons::get(QStringLiteral("mountain")));
  tool_create_junction->setIcon(Icons::get(QStringLiteral("junction-connect")));
  tool_split->setIcon(Icons::get(QStringLiteral("scissors")));
  merge_roads->setIcon(Icons::get(QStringLiteral("git-merge")));
  tool_delete->setIcon(Icons::get(QStringLiteral("trash-2")));
  tool_lane_add->setIcon(Icons::get(QStringLiteral("lane-section")));
  tool_lane_form->setIcon(Icons::get(QStringLiteral("lane-section")));
  template_rural->setIcon(Icons::get(QStringLiteral("template-rural")));
  template_urban->setIcon(Icons::get(QStringLiteral("template-urban")));
  template_highway->setIcon(Icons::get(QStringLiteral("template-highway")));
  reset_camera->setIcon(Icons::get(QStringLiteral("rotate-ccw")));
  frame_selection->setIcon(Icons::get(QStringLiteral("scan")));
  add_from_library->setIcon(Icons::get(QStringLiteral("circle-plus")));
}

} // namespace roadmaker::editor
