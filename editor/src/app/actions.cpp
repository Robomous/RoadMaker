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
  export_glb->setShortcuts(shortcuts::sequences(shortcuts::Id::ExportGlb));
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

  tool_lane_carve = new QAction(tr("Lane &Carve"), this);
  tool_lane_carve->setCheckable(true);
  tool_lane_carve->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolLaneCarve));
  tool_lane_carve->setIconText(tr("Lane Carve"));
  tool_lane_carve->setToolTip(
      tr("Lane Carve — drag along a lane toward a junction to carve a tapering turn lane (⇧C)"));
  tool_group->addAction(tool_lane_carve);

  tool_crosswalk = new QAction(tr("&Crosswalk && Stop Line"), this);
  tool_crosswalk->setCheckable(true);
  tool_crosswalk->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolCrosswalk));
  tool_crosswalk->setIconText(tr("Crosswalk"));
  tool_crosswalk->setToolTip(
      tr("Crosswalk & Stop Line — click a junction approach to place a crosswalk and its "
         "stop line, or drag a crosswalk asset from the Library onto an approach (W)"));
  tool_group->addAction(tool_crosswalk);

  tool_marking_point = new QAction(tr("Marking &Point"), this);
  tool_marking_point->setCheckable(true);
  tool_marking_point->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolMarkingPoint));
  tool_marking_point->setIconText(tr("Marking Point"));
  tool_marking_point->setToolTip(
      tr("Marking Point — click a lane to place the selected arrow stencil, or drag a stencil "
         "asset from the Library onto a lane (S)"));
  tool_group->addAction(tool_marking_point);

  tool_marking_curve = new QAction(tr("Marking Cur&ve"), this);
  tool_marking_curve->setCheckable(true);
  tool_marking_curve->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolMarkingCurve));
  tool_marking_curve->setIconText(tr("Marking Curve"));
  tool_marking_curve->setToolTip(
      tr("Marking Curve — click along a road to draw a free-form line marking or crossing; "
         "Enter commits, Backspace undoes a point, Esc cancels (⇧W)"));
  tool_group->addAction(tool_marking_curve);

  tool_prop_point = new QAction(tr("Prop Poin&t"), this);
  tool_prop_point->setCheckable(true);
  tool_prop_point->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolPropPoint));
  tool_prop_point->setIconText(tr("Prop Point"));
  tool_prop_point->setToolTip(
      tr("Prop Point — click on or beside a road to place the selected tree/shrub, or drag a "
         "prop asset from the Library onto a road; drag a placed prop to move it (T)"));
  tool_group->addAction(tool_prop_point);

  tool_prop_curve = new QAction(tr("Prop Cur&ve"), this);
  tool_prop_curve->setCheckable(true);
  tool_prop_curve->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolPropCurve));
  tool_prop_curve->setIconText(tr("Prop Curve"));
  tool_prop_curve->setToolTip(
      tr("Prop Curve — click along a road to distribute props at a fixed spacing; "
         "[ / ] adjust spacing, Enter bakes them into individual props, Esc cancels (⇧T)"));
  tool_group->addAction(tool_prop_curve);

  tool_prop_span = new QAction(tr("Prop Spa&n"), this);
  tool_prop_span->setCheckable(true);
  tool_prop_span->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolPropSpan));
  tool_prop_span->setIconText(tr("Prop Span"));
  tool_prop_span->setToolTip(
      tr("Prop Span — click two stations on one road to place a repeating run of the selected "
         "prop; [ / ] adjust spacing, Enter commits, Esc cancels (⇧S)"));
  tool_group->addAction(tool_prop_span);

  tool_prop_polygon = new QAction(tr("Prop Pol&ygon"), this);
  tool_prop_polygon->setCheckable(true);
  tool_prop_polygon->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolPropPolygon));
  tool_prop_polygon->setIconText(tr("Prop Polygon"));
  tool_prop_polygon->setToolTip(
      tr("Prop Polygon — outline a region to scatter the selected prop across it; [ / ] adjust "
         "density, R re-scatters, Enter bakes them into individual props, Esc cancels (⇧P)"));
  tool_group->addAction(tool_prop_polygon);

  tool_corner = new QAction(tr("Co&rner"), this);
  tool_corner->setCheckable(true);
  tool_corner->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolCorner));
  tool_corner->setIconText(tr("Corner"));
  tool_corner->setToolTip(
      tr("Corner — click a junction corner, then drag its apex to set the fillet radius or a "
         "tangency point to set that side's extent, Esc cancels (⇧R)"));
  tool_group->addAction(tool_corner);

  tool_stopline = new QAction(tr("St&op Line"), this);
  tool_stopline->setCheckable(true);
  tool_stopline->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolStopLine));
  tool_stopline->setIconText(tr("Stop Line"));
  tool_stopline->setToolTip(
      tr("Stop Line — click a junction stop line, then drag it along the arm to set its "
         "distance, F flips which direction it spans, Esc cancels (\u21e7O)"));
  tool_group->addAction(tool_stopline);

  tool_junction_span = new QAction(tr("&Junction Span"), this);
  tool_junction_span->setCheckable(true);
  tool_junction_span->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolJunctionSpan));
  tool_junction_span->setIconText(tr("Junction Span"));
  tool_junction_span->setToolTip(
      tr("Junction Span — drag along a road to mark the stretch a virtual junction covers "
         "(\u00a712.7); drag a parallel road for a second span, Enter creates it, Esc resets "
         "(\u21e7J)"));
  tool_group->addAction(tool_junction_span);

  tool_junction_surface = new QAction(tr("Junction &Surface"), this);
  tool_junction_surface->setCheckable(true);
  tool_junction_surface->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolJunctionSurface));
  tool_junction_surface->setIconText(tr("Junction Surface"));
  tool_junction_surface->setToolTip(
      tr("Junction Surface \u2014 inspect the spans the junction floor is built from; Space "
         "toggles a span's samples, PgUp/PgDn raise or lower it on overlap (I)"));
  tool_group->addAction(tool_junction_surface);

  tool_maneuver = new QAction(tr("&Maneuver"), this);
  tool_maneuver->setCheckable(true);
  tool_maneuver->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolManeuver));
  tool_maneuver->setIconText(tr("Maneuver"));
  tool_maneuver->setToolTip(
      tr("Maneuver \u2014 click a junction turn, then drag its points to reshape it or an "
         "endpoint to slide it across the arm; Del removes a point, Esc cancels (\u21e7M)"));
  tool_group->addAction(tool_maneuver);

  tool_signal = new QAction(tr("Si&gnal"), this);
  tool_signal->setCheckable(true);
  tool_signal->setShortcuts(shortcuts::sequences(shortcuts::Id::ToolSignal));
  tool_signal->setIconText(tr("Signal"));
  tool_signal->setToolTip(
      tr("Signal \u2014 click a junction to auto-signalize it from a template, or a road to place "
         "one signal from the Library (G)"));
  tool_group->addAction(tool_signal);

  // Not a tool — a command that surfaces the 2D Editor's Lane Width tab for the
  // selected lane. Standalone so ⇧L works whatever tool is active.
  lane_width_editor = new QAction(tr("Lane &Width Editor"), this);
  lane_width_editor->setShortcuts(shortcuts::sequences(shortcuts::Id::LaneWidthEditor));
  lane_width_editor->setIconText(tr("Width"));
  lane_width_editor->setToolTip(tr("Open the 2D Editor's Lane Width tab for the selected "
                                   "lane — drag the width curve along s (⇧L)"));

  // Not a tool — a command that surfaces the 2D Editor's Signal Phases tab for
  // the selected junction. Standalone so ⇧G works whatever tool is active.
  signal_phase_editor = new QAction(tr("Signal &Phase Editor"), this);
  signal_phase_editor->setShortcuts(shortcuts::sequences(shortcuts::Id::SignalPhaseEditor));
  signal_phase_editor->setIconText(tr("Phases"));
  signal_phase_editor->setToolTip(tr("Open the 2D Editor's Signal Phases tab for the selected "
                                     "junction — scrub the red-yellow-green cycle (⇧G)"));

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
  reset_camera->setShortcuts(shortcuts::sequences(shortcuts::Id::ResetCamera));
  reset_camera->setIconText(tr("Camera"));
  reset_camera->setToolTip(tr("Reset the camera to the default view"));
  add_from_library = new QAction(tr("Add from &Library…"), this);
  add_from_library->setShortcuts(shortcuts::sequences(shortcuts::Id::AddFromLibrary));
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
  // View ▸ Viewport Hints (#333). Checkable and menu-only; MainWindow owns the
  // checked state (it is persisted in QSettings, not derived from the tool).
  viewport_hints = new QAction(tr("&Viewport Hints"), this);
  viewport_hints->setShortcuts(shortcuts::sequences(shortcuts::Id::ViewportHints));
  viewport_hints->setCheckable(true);
  viewport_hints->setToolTip(
      tr("Show the active tool's hint in the viewport corner (H) — the status bar keeps it "
         "either way"));

  merge_roads = new QAction(tr("&Merge Roads"), this);
  merge_roads->setShortcuts(shortcuts::sequences(shortcuts::Id::MergeRoads));
  merge_roads->setIconText(tr("Merge"));
  merge_roads->setEnabled(false); // enabled only for a mergeable 2-road selection
  merge_roads->setToolTip(tr("Merge — join two selected roads that meet end-to-start"));

  reset_layout = new QAction(tr("Reset &Layout"), this);

  help_contents = new QAction(tr("&User Guide"), this);
  help_contents->setShortcuts(shortcuts::sequences(shortcuts::Id::Help));
  help_contents->setToolTip(tr("Open the RoadMaker user guide (F1)"));

  about = new QAction(tr("&About RoadMaker"), this);
  about->setMenuRole(QAction::AboutRole);

  apply_icons();
}

QAction* Actions::action(shortcuts::Id id) const {
  using shortcuts::Id;
  // No `default:` on purpose — -Wswitch -Werror is what forces a new Id to be
  // mapped here before it can compile (see the header).
  switch (id) {
  case Id::NewScene:
    return new_file;
  case Id::Open:
    return open;
  case Id::Save:
    return save;
  case Id::SaveAs:
    return save_as;
  case Id::ExportGlb:
    return export_glb;
  case Id::Quit:
    return quit;
  case Id::Undo:
    return undo;
  case Id::Redo:
    return redo;
  case Id::ToolSelect:
    return tool_select;
  case Id::ToolMove:
    return tool_move;
  case Id::ToolCreateRoad:
    return tool_create_road;
  case Id::ToolEditNodes:
    return tool_edit_nodes;
  case Id::ToolLaneProfile:
    return tool_lane_profile;
  case Id::ToolElevation:
    return tool_elevation;
  case Id::ToolCreateJunction:
    return tool_create_junction;
  case Id::ToolSplit:
    return tool_split;
  case Id::ToolDelete:
    return tool_delete;
  case Id::ToolLaneAdd:
    return tool_lane_add;
  case Id::ToolLaneForm:
    return tool_lane_form;
  case Id::ToolLaneCarve:
    return tool_lane_carve;
  case Id::ToolCrosswalk:
    return tool_crosswalk;
  case Id::ToolMarkingPoint:
    return tool_marking_point;
  case Id::ToolMarkingCurve:
    return tool_marking_curve;
  case Id::ToolPropPoint:
    return tool_prop_point;
  case Id::ToolPropCurve:
    return tool_prop_curve;
  case Id::ToolPropSpan:
    return tool_prop_span;
  case Id::ToolPropPolygon:
    return tool_prop_polygon;
  case Id::ToolCorner:
    return tool_corner;
  case Id::ToolStopLine:
    return tool_stopline;
  case Id::ToolJunctionSpan:
    return tool_junction_span;
  case Id::ToolJunctionSurface:
    return tool_junction_surface;
  case Id::ToolManeuver:
    return tool_maneuver;
  case Id::ToolSignal:
    return tool_signal;
  case Id::LaneWidthEditor:
    return lane_width_editor;
  case Id::SignalPhaseEditor:
    return signal_phase_editor;
  case Id::MergeRoads:
    return merge_roads;
  case Id::AddFromLibrary:
    return add_from_library;
  case Id::ResetCamera:
    return reset_camera;
  case Id::FrameSelection:
    return frame_selection;
  case Id::FrameCursor:
    return frame_cursor;
  case Id::ViewPerspective:
    return view_perspective;
  case Id::ViewOrthographic:
    return view_orthographic;
  case Id::ViewNorth:
    return view_north;
  case Id::ViewSouth:
    return view_south;
  case Id::ViewWest:
    return view_west;
  case Id::ViewEast:
    return view_east;
  case Id::ViewTop:
    return view_top;
  case Id::ViewportHints:
    return viewport_hints;
  case Id::Help:
    return help_contents;
  case Id::kIdCount:
    break; // the sentinel names no action
  }
  return nullptr;
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
  tool_lane_carve->setIcon(Icons::get(QStringLiteral("lane-section")));
  tool_crosswalk->setIcon(Icons::get(QStringLiteral("crosswalk")));
  tool_marking_point->setIcon(Icons::get(QStringLiteral("marking-point")));
  tool_marking_curve->setIcon(Icons::get(QStringLiteral("marking-curve")));
  tool_prop_point->setIcon(Icons::get(QStringLiteral("prop-point")));
  tool_prop_curve->setIcon(Icons::get(QStringLiteral("prop-curve")));
  tool_prop_span->setIcon(Icons::get(QStringLiteral("prop-span")));
  tool_prop_polygon->setIcon(Icons::get(QStringLiteral("prop-polygon")));
  tool_corner->setIcon(Icons::get(QStringLiteral("corner-radius")));
  tool_stopline->setIcon(Icons::get(QStringLiteral("stop-line")));
  tool_junction_span->setIcon(Icons::get(QStringLiteral("junction-span")));
  tool_junction_surface->setIcon(Icons::get(QStringLiteral("junction-surface")));
  tool_maneuver->setIcon(Icons::get(QStringLiteral("maneuver")));
  tool_signal->setIcon(Icons::get(QStringLiteral("signal")));
  template_rural->setIcon(Icons::get(QStringLiteral("template-rural")));
  template_urban->setIcon(Icons::get(QStringLiteral("template-urban")));
  template_highway->setIcon(Icons::get(QStringLiteral("template-highway")));
  reset_camera->setIcon(Icons::get(QStringLiteral("rotate-ccw")));
  frame_selection->setIcon(Icons::get(QStringLiteral("scan")));
  add_from_library->setIcon(Icons::get(QStringLiteral("circle-plus")));
}

} // namespace roadmaker::editor
