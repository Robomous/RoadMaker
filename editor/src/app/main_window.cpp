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

#include "app/main_window.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/bridge.hpp"
#include "roadmaker/road/grade_separation.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/version.hpp"
#include "roadmaker/xodr/terrain_sidecar.hpp"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

#include "app/context_menu.hpp"
#include "app/crash_handler.hpp"
#include "app/icons.hpp"
#include "app/log_setup.hpp"
#include "app/shortcut_registry.hpp"
#include "app/tour_controller.hpp"
#include "app/tour_overlay.hpp"
#include "document/crosswalk_item.hpp"
#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"
#include "document/signal_phase_overlay.hpp"
#include "document/units.hpp"
#include "help/help_registry.hpp"
#include "help/help_viewer.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/editor2d_host.hpp"
#include "panels/library_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"
#include "panels/unit_spin_box.hpp"
#include "tools/corner_tool.hpp"
#include "tools/create_junction_tool.hpp"
#include "tools/create_road_tool.hpp"
#include "tools/crosswalk_stop_line_tool.hpp"
#include "tools/delete_tool.hpp"
#include "tools/edit_nodes_tool.hpp"
#include "tools/elevation_tool.hpp"
#include "tools/junction_span_tool.hpp"
#include "tools/junction_surface_tool.hpp"
#include "tools/lane_add_tool.hpp"
#include "tools/lane_carve_tool.hpp"
#include "tools/lane_form_tool.hpp"
#include "tools/lane_profile_tool.hpp"
#include "tools/maneuver_tool.hpp"
#include "tools/marking_curve_tool.hpp"
#include "tools/marking_point_tool.hpp"
#include "tools/prop_curve_tool.hpp"
#include "tools/prop_point_tool.hpp"
#include "tools/prop_polygon_tool.hpp"
#include "tools/prop_span_tool.hpp"
#include "tools/select_tool.hpp"
#include "tools/sign_tool.hpp"
#include "tools/signal_tool.hpp"
#include "tools/split_tool.hpp"
#include "tools/stopline_tool.hpp"
#include "tools/surface_tool.hpp"
#include "tools/terrain_brush_tool.hpp"

namespace roadmaker::editor {

MainWindow::MainWindow(QWidget* parent, bool restore_saved_layout)
    : QMainWindow(parent), autosave_(document_,
                                     AutosaveManager::default_recovery_dir(),
                                     QUuid::createUuid().toString(QUuid::WithoutBraces)),
      selection_(document_), scene_tree_model_(document_), diagnostics_model_(document_),
      actions_(new Actions(*document_.undo_stack(), this)),
      central_stack_(new QStackedWidget(this)), welcome_(new WelcomeWidget(settings_, this)),
      viewport_(new ViewportWidget(document_, selection_, tool_manager_, this)),
      status_hover_(new QLabel(this)), status_entities_(new QLabel(this)) {
  // Seed the display-unit system from Settings before any panel or readout
  // formats its first value (#412) — everything below renders through it.
  units::set_active(settings_.display_units());
  setAcceptDrops(true);
  allow_first_run_tour_ = restore_saved_layout; // suppressed for capture windows
  // Central stack: the welcome screen greets an empty session; any document
  // activity (New/Open/recent/sample/recovery) flips to the viewport for
  // the rest of the session. Never dockable.
  central_stack_->addWidget(welcome_);
  central_stack_->addWidget(viewport_);
  central_stack_->setCurrentWidget(welcome_);
  setCentralWidget(central_stack_);
  resize(1600, 1000);
  // Two toolbar rows need more width before they start collapsing into the
  // extension arrow. Set on the WINDOW, never on the toolbars: QToolBar's tiny
  // minimumSizeHint is exactly what makes that arrow work.
  setMinimumSize(QSize(920, 600));

  connect(welcome_, &WelcomeWidget::new_scene_requested, this, [this] { new_file(); });
  connect(welcome_, &WelcomeWidget::open_requested, this, &MainWindow::open_file_dialog);
  connect(welcome_, &WelcomeWidget::file_requested, this, [this](const QString& path) {
    load_file(std::filesystem::path(path.toStdString()));
  });
  // A project tile opens the project (Library overlay, recents, title) and
  // repopulates the welcome view with that project's scenes — no document
  // loads until the user picks one.
  connect(welcome_, &WelcomeWidget::project_requested, this, &MainWindow::open_project_dir);
  connect(
      &document_, &Document::loaded, this, [this] { central_stack_->setCurrentWidget(viewport_); });

  build_docks();
  build_menus();
  build_toolbar();
  build_tool_options_bar();
  build_status_bar();

  connect(actions_->new_file, &QAction::triggered, this, &MainWindow::new_file);
  connect(actions_->open, &QAction::triggered, this, &MainWindow::open_file_dialog);
  connect(actions_->save, &QAction::triggered, this, [this] { save_file(); });
  connect(actions_->save_as, &QAction::triggered, this, [this] { save_file_as(); });
  connect(actions_->export_glb, &QAction::triggered, this, &MainWindow::export_file_dialog);
#ifdef RM_HAVE_USD
  connect(actions_->export_usd, &QAction::triggered, this, &MainWindow::export_usd_dialog);
#endif
  connect(actions_->quit, &QAction::triggered, this, &QMainWindow::close);
  connect(actions_->reset_camera, &QAction::triggered, viewport_, &ViewportWidget::reset_camera);
  connect(
      actions_->frame_selection, &QAction::triggered, viewport_, &ViewportWidget::frame_selection);
  connect(actions_->frame_cursor, &QAction::triggered, viewport_, &ViewportWidget::frame_cursor);
  connect(actions_->view_perspective, &QAction::triggered, viewport_, [this] {
    viewport_->set_projection(ProjectionMode::Perspective);
  });
  connect(actions_->view_orthographic, &QAction::triggered, viewport_, [this] {
    viewport_->set_projection(ProjectionMode::Orthographic);
  });
  const auto bind_cardinal = [this](QAction* action, CardinalView view) {
    connect(action, &QAction::triggered, viewport_, [this, view] { viewport_->look_from(view); });
  };
  bind_cardinal(actions_->view_north, CardinalView::North);
  bind_cardinal(actions_->view_south, CardinalView::South);
  bind_cardinal(actions_->view_west, CardinalView::West);
  bind_cardinal(actions_->view_east, CardinalView::East);
  bind_cardinal(actions_->view_top, CardinalView::Top);
  connect(actions_->add_from_library, &QAction::triggered, this, [this] {
    library_dock_->show();
    library_dock_->raise();
  });
  connect(actions_->about, &QAction::triggered, this, &MainWindow::show_about_dialog);
  connect(viewport_, &ViewportWidget::hover_changed, this, &MainWindow::on_hover);
  connect(viewport_, &ViewportWidget::library_item_dropped, this, &MainWindow::on_library_drop);
  connect(viewport_,
          &ViewportWidget::library_item_drag_moved,
          this,
          &MainWindow::on_library_drag_moved);
  connect(viewport_,
          &ViewportWidget::context_menu_requested,
          this,
          [this](const MenuContext& context, const QPoint& global_pos) {
            ContextMenuDeps deps{document_, selection_, *actions_};
            deps.default_crosswalk_params = [this] { return resolve_default_crosswalk_params(); };
            deps.open_signal_phase_editor = [this](JunctionId) {
              editor2d_dock_->show();
              editor2d_dock_->raise();
              editor2d_host_->show_page(tr("Signal Phases"));
            };
            if (QMenu* menu = assemble_context_menu(context, deps, this)) {
              menu->popup(global_pos);
            }
          });
  connect(&document_, &Document::loaded, this, [this] {
    actions_->export_glb->setEnabled(true);
#ifdef RM_HAVE_USD
    actions_->export_usd->setEnabled(true);
#endif
    update_window_title();
    update_status_entities();
    diagnostics_dock_->setVisible(!document_.diagnostics().empty() ||
                                  diagnostics_dock_->isVisible());
  });
  // QUndoStack::isClean drives the modified flag; the [*] placeholder in
  // the window title renders it (§8).
  connect(document_.undo_stack(), &QUndoStack::cleanChanged, this, [this](bool clean) {
    setWindowModified(!clean);
  });
  connect(&document_, &Document::saved, this, &MainWindow::update_window_title);
  connect(&document_, &Document::regeneration_skipped, this, [this](const QString& reason) {
    viewport_->show_toast(tr("Junction not updated: %1").arg(reason), ToastSeverity::Warning);
  });

  // Autosave tick — the debounce and the recover-vs-clean decision live in
  // AutosaveManager (fake-clock testable, §3); this timer is the thin
  // widget-side wrapper. Ticking faster than kIntervalMs keeps the actual
  // debounce inside maybe_autosave().
  autosave_.set_enabled(settings_.autosave_enabled());
  auto* autosave_timer = new QTimer(this);
  autosave_timer->setInterval(5'000);
  connect(autosave_timer, &QTimer::timeout, &autosave_, &AutosaveManager::maybe_autosave);
  autosave_timer->start();
  // After the event loop is up: surface any crash report from an earlier
  // session first (context for what follows), then offer to recover that
  // session's unsaved work.
  QTimer::singleShot(0, this, &MainWindow::check_crash_reports);
  QTimer::singleShot(0, this, &MainWindow::check_recovery);

  // Editing tools (M2). Select/Move is the default; guidance lands in the
  // status bar via the tool's status_message AND as the viewport corner
  // hint — during a tool interaction the user's eyes are on the viewport,
  // not the status bar (issue #103 discoverability).
  const auto wire_status = [this](Tool* tool) {
    connect(tool, &Tool::status_message, this, [this](const QString& text) {
      statusBar()->showMessage(text, 5000);
      viewport_->set_hint(text);
    });
    connect(
        tool, &Tool::toast_requested, this, [this](const QString& text, ToastSeverity severity) {
          viewport_->show_toast(text, severity);
        });
    // One-shot tools ask to return to another tool; trigger the matching action
    // so its checkable toolbar state follows (Select is the only target today).
    connect(tool, &Tool::request_tool, this, [this](ToolId id) {
      if (id == ToolId::Select) {
        actions_->tool_select->trigger();
      } else {
        tool_manager_.set_active(id);
      }
    });
  };
  // Moving a road that links to roads staying put breaks those links. Confirm
  // once (with a session-wide "don't ask again"), BEFORE the preview begins —
  // a modal opened mid-drag swallows the mouse-release. Shared by the Select
  // tool (power path) and the Move tool (discoverable path).
  const auto link_break_confirm = [this]() -> bool {
    if (suppress_link_break_confirm_) {
      return true;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Break road links?"));
    box.setText(tr("Moving this road will break its connection to roads that stay put."));
    box.setInformativeText(tr("Move it anyway?"));
    auto* dont_ask = new QCheckBox(tr("Don't ask again this session"), &box);
    box.setCheckBox(dont_ask);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Ok);
    const int result = box.exec();
    if (dont_ask->isChecked()) {
      suppress_link_break_confirm_ = true;
    }
    return result == QMessageBox::Ok;
  };
  auto select_tool = std::make_unique<SelectTool>(document_, selection_);
  wire_status(select_tool.get());
  select_tool->set_link_break_confirm(link_break_confirm);
  SelectTool* select_tool_ptr = select_tool.get();
  tool_manager_.register_tool(ToolId::Select, std::move(select_tool));
  connect(actions_->tool_select, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Select);
  });

  // The Move tool: the same Select/Move machinery in "move mode" — a discoverable
  // toolbar entry with a 4-arrow hover cursor and always-move body drag (no
  // rubber band). Select stays the power path (#176).
  auto move_tool = std::make_unique<SelectTool>(document_, selection_);
  move_tool->set_move_mode(true);
  wire_status(move_tool.get());
  move_tool->set_link_break_confirm(link_break_confirm);
  tool_manager_.register_tool(ToolId::Move, std::move(move_tool));
  connect(actions_->tool_move, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Move);
  });
  auto create_road_tool = std::make_unique<CreateRoadTool>(document_);
  create_road_tool_ = create_road_tool.get();
  wire_status(create_road_tool.get());
  tool_manager_.register_tool(ToolId::CreateRoad, std::move(create_road_tool));
  connect(actions_->tool_create_road, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::CreateRoad);
  });
  // Feed the single-road selection to Create Road so a first click on that
  // road's END extends it (instead of authoring a new welded road).
  connect(&selection_, &SelectionModel::selection_changed, this, [this] {
    if (create_road_tool_ == nullptr) {
      return;
    }
    const std::vector<RoadId> roads = selection_.selected_roads();
    create_road_tool_->set_selected_road(roads.size() == 1 ? std::optional<RoadId>(roads.front())
                                                           : std::nullopt);
  });
  // Picking a template arms the Create Road tool with it and switches to
  // the tool — choosing a cross section IS the intent to draw one.
  const auto arm_template = [this](const LaneProfile& profile) {
    create_road_tool_->set_profile(profile);
    actions_->tool_create_road->setChecked(true);
    tool_manager_.set_active(ToolId::CreateRoad);
  };
  connect(actions_->template_rural, &QAction::triggered, this, [arm_template] {
    arm_template(LaneProfile::two_lane_rural());
  });
  connect(actions_->template_urban, &QAction::triggered, this, [arm_template] {
    arm_template(LaneProfile::urban_sidewalk());
  });
  connect(actions_->template_highway, &QAction::triggered, this, [arm_template] {
    arm_template(LaneProfile::highway());
  });
  auto edit_nodes_tool = std::make_unique<EditNodesTool>(document_, selection_);
  wire_status(edit_nodes_tool.get());
  EditNodesTool* edit_nodes_ptr = edit_nodes_tool.get();
  tool_manager_.register_tool(ToolId::EditNodes, std::move(edit_nodes_tool));
  connect(actions_->tool_edit_nodes, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::EditNodes);
  });
  // Double-clicking a road body in Select inserts a bend node and hands off to
  // Edit Nodes with the fresh node grabbed — double-click-then-drag to shape it.
  connect(select_tool_ptr,
          &SelectTool::edit_nodes_requested,
          this,
          [this, edit_nodes_ptr](RoadId road, std::size_t index) {
            actions_->tool_edit_nodes->setChecked(true);
            tool_manager_.set_active(ToolId::EditNodes);
            edit_nodes_ptr->adopt_node(road, index);
          });
  auto lane_profile_tool = std::make_unique<LaneProfileTool>(document_, selection_);
  wire_status(lane_profile_tool.get());
  tool_manager_.register_tool(ToolId::LaneProfile, std::move(lane_profile_tool));
  connect(actions_->tool_lane_profile, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::LaneProfile);
    // Surface the 2D Editor pane so the Lane Width tab is reachable when the
    // lane workflow starts (mirrors the Elevation tool; discoverability rule).
    editor2d_dock_->show();
    editor2d_dock_->raise();
    editor2d_host_->raise_relevant_page();
  });
  // ⇧L: jump straight to the Lane Width tab (whatever tool is active).
  connect(actions_->lane_width_editor, &QAction::triggered, this, [this] {
    editor2d_dock_->show();
    editor2d_dock_->raise();
    editor2d_host_->show_page(tr("Lane Width"));
  });
  // ⇧G: jump straight to the Signal Phases tab (whatever tool is active). The
  // literal must match SignalPhaseEditorPage::title() exactly.
  connect(actions_->signal_phase_editor, &QAction::triggered, this, [this] {
    editor2d_dock_->show();
    editor2d_dock_->raise();
    editor2d_host_->show_page(tr("Signal Phases"));
  });
  auto elevation_tool = std::make_unique<ElevationTool>(document_, selection_);
  elevation_tool_ = elevation_tool.get();
  wire_status(elevation_tool.get());
  tool_manager_.register_tool(ToolId::Elevation, std::move(elevation_tool));
  connect(actions_->tool_elevation, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Elevation);
    // Surface the 2D Editor pane (vertical-profile handles + the overpass Cross
    // Over/Under controls) when the elevation workflow starts — otherwise it
    // hides behind a View-menu toggle (discoverability rule, product-parity.md).
    editor2d_dock_->show();
    editor2d_dock_->raise();
    editor2d_host_->raise_relevant_page();
  });
  // The Properties panel edits the node the Elevation tool has made active.
  properties_panel_->set_elevation_tool(elevation_tool_);
  auto create_junction_tool = std::make_unique<CreateJunctionTool>(document_);
  wire_status(create_junction_tool.get());
  tool_manager_.register_tool(ToolId::CreateJunction, std::move(create_junction_tool));
  connect(actions_->tool_create_junction, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::CreateJunction);
  });
  auto split_tool = std::make_unique<SplitTool>(document_, selection_);
  wire_status(split_tool.get());
  tool_manager_.register_tool(ToolId::Split, std::move(split_tool));
  connect(actions_->tool_split, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Split);
  });
  auto delete_tool = std::make_unique<DeleteTool>(document_);
  wire_status(delete_tool.get());
  tool_manager_.register_tool(ToolId::Delete, std::move(delete_tool));
  connect(actions_->tool_delete, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Delete);
  });
  auto lane_add_tool = std::make_unique<LaneAddTool>(document_, selection_);
  wire_status(lane_add_tool.get());
  tool_manager_.register_tool(ToolId::LaneAdd, std::move(lane_add_tool));
  connect(actions_->tool_lane_add, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::LaneAdd);
  });
  auto lane_form_tool = std::make_unique<LaneFormTool>(document_, selection_);
  wire_status(lane_form_tool.get());
  tool_manager_.register_tool(ToolId::LaneForm, std::move(lane_form_tool));
  connect(actions_->tool_lane_form, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::LaneForm);
  });
  auto lane_carve_tool = std::make_unique<LaneCarveTool>(document_, selection_);
  wire_status(lane_carve_tool.get());
  tool_manager_.register_tool(ToolId::LaneCarve, std::move(lane_carve_tool));
  connect(actions_->tool_lane_carve, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::LaneCarve);
  });
  auto crosswalk_tool = std::make_unique<CrosswalkStopLineTool>(document_, selection_);
  wire_status(crosswalk_tool.get());
  // The tool places the merged Library's default crosswalk asset, so a click on
  // an approach carries the same parameters the context-menu generator uses.
  crosswalk_tool->set_params_provider([this] { return resolve_default_crosswalk_params(); });
  tool_manager_.register_tool(ToolId::Crosswalk, std::move(crosswalk_tool));
  connect(actions_->tool_crosswalk, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Crosswalk);
  });
  auto marking_point_tool = std::make_unique<MarkingPointTool>(document_, selection_);
  wire_status(marking_point_tool.get());
  // The tool places the merged Library's default Stencil asset, so a click on a
  // lane carries the same glyph the Library drag-drop path lands.
  marking_point_tool->set_params_provider([this] { return resolve_default_stencil_item(); });
  tool_manager_.register_tool(ToolId::MarkingPoint, std::move(marking_point_tool));
  connect(actions_->tool_marking_point, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::MarkingPoint);
  });
  auto marking_curve_tool = std::make_unique<MarkingCurveTool>(document_, selection_);
  wire_status(marking_curve_tool.get());
  // The tool authors the merged Library's default crosswalk/marking asset.
  marking_curve_tool->set_params_provider([this] { return resolve_default_marking_curve_item(); });
  tool_manager_.register_tool(ToolId::MarkingCurve, std::move(marking_curve_tool));
  connect(actions_->tool_marking_curve, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::MarkingCurve);
  });
  auto prop_point_tool = std::make_unique<PropPointTool>(document_, selection_);
  wire_status(prop_point_tool.get());
  // The tool places the merged Library's default prop asset, so a click carries
  // the same tree/shrub the Library drag-drop path lands.
  prop_point_tool->set_params_provider([this] { return resolve_default_prop_item(); });
  tool_manager_.register_tool(ToolId::PropPoint, std::move(prop_point_tool));
  connect(actions_->tool_prop_point, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::PropPoint);
  });
  auto prop_curve_tool = std::make_unique<PropCurveTool>(document_, selection_);
  wire_status(prop_curve_tool.get());
  // The tool distributes the merged Library's default prop asset along the curve.
  prop_curve_tool->set_params_provider([this] { return resolve_default_prop_item(); });
  tool_manager_.register_tool(ToolId::PropCurve, std::move(prop_curve_tool));
  connect(actions_->tool_prop_curve, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::PropCurve);
  });
  auto prop_span_tool = std::make_unique<PropSpanTool>(document_, selection_);
  wire_status(prop_span_tool.get());
  // The tool repeats the merged Library's default prop along the span.
  prop_span_tool->set_params_provider([this] { return resolve_default_prop_item(); });
  tool_manager_.register_tool(ToolId::PropSpan, std::move(prop_span_tool));
  connect(actions_->tool_prop_span, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::PropSpan);
  });
  auto prop_polygon_tool = std::make_unique<PropPolygonTool>(document_, selection_);
  wire_status(prop_polygon_tool.get());
  // The tool scatters the merged Library's default prop across the region.
  prop_polygon_tool->set_params_provider([this] { return resolve_default_prop_item(); });
  tool_manager_.register_tool(ToolId::PropPolygon, std::move(prop_polygon_tool));
  connect(actions_->tool_prop_polygon, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::PropPolygon);
  });
  // Corner: junction fillets. No params provider — it edits what it picks.
  auto corner_tool = std::make_unique<CornerTool>(document_, selection_);
  wire_status(corner_tool.get());
  // The Properties pane edits the corner the tool has made active.
  properties_panel_->set_corner_tool(corner_tool.get());
  tool_manager_.register_tool(ToolId::Corner, std::move(corner_tool));

  // Stop Line: the derived band on each junction arm. Like the Corner tool it
  // edits what it picks, and the Properties pane binds to its sub-selection.
  auto stopline_tool = std::make_unique<StopLineTool>(document_, selection_);
  wire_status(stopline_tool.get());
  properties_panel_->set_stopline_tool(stopline_tool.get());
  tool_manager_.register_tool(ToolId::StopLine, std::move(stopline_tool));

  // Junction Span: authors an ASAM 12.7 VIRTUAL junction over a stretch of road.
  // It creates rather than edits, so nothing binds to it from the panels.
  auto junction_span_tool = std::make_unique<JunctionSpanTool>(document_, selection_);
  wire_status(junction_span_tool.get());
  tool_manager_.register_tool(ToolId::JunctionSpan, std::move(junction_span_tool));

  // Junction Surface: the floor's per-connecting-road spans (p4-s5, #320).
  // Inspection only — no drag, no preview session; it follows the SELECTION
  // rather than picking a junction of its own, and the Properties pane binds to
  // its sub-selection so a row click and a viewport click agree.
  auto junction_surface_tool = std::make_unique<JunctionSurfaceTool>(document_, selection_);
  wire_status(junction_surface_tool.get());
  properties_panel_->set_junction_surface_tool(junction_surface_tool.get());
  tool_manager_.register_tool(ToolId::JunctionSurface, std::move(junction_surface_tool));

  // Maneuver: one connecting road's path through a junction (p4-s6, #227). It
  // edits what it picks (there is no mesh proxy for a connecting road, so it
  // hit-tests the sampled paths in screen space), and the Properties pane binds
  // to its sub-selection so a row click and a viewport click agree.
  auto maneuver_tool = std::make_unique<ManeuverTool>(document_, selection_);
  wire_status(maneuver_tool.get());
  properties_panel_->set_maneuver_tool(maneuver_tool.get());
  tool_manager_.register_tool(ToolId::Maneuver, std::move(maneuver_tool));

  // Signal: the signalization layer (p4-s7, #228). It targets a junction so the
  // Properties pane can apply a template, selects a placed head, and places one
  // signal from the Library on a road — all through Document commands.
  auto signal_tool = std::make_unique<SignalTool>(document_, selection_);
  wire_status(signal_tool.get());
  signal_tool->set_params_provider([this] { return resolve_default_signal_item(); });
  properties_panel_->set_signal_tool(signal_tool.get());
  tool_manager_.register_tool(ToolId::Signal, std::move(signal_tool));

  // Sign: placement-only sibling (p4-s9, #230). Places one road sign — a
  // selected Library sign, or by default a StVO 310 text plate — then selects it
  // so the Attributes pane edits its face text.
  auto sign_tool = std::make_unique<SignTool>(document_, selection_);
  wire_status(sign_tool.get());
  sign_tool->set_params_provider([this] { return resolve_default_sign_item(); });
  tool_manager_.register_tool(ToolId::Sign, std::move(sign_tool));

  // Surface: a ground surface's boundary as a node graph (p5-s1, #231). Like
  // EditNodes it follows the SELECTION, and like the junction lock its first
  // edit DETACHES a derived surface to authored (decision D3) — the Attributes
  // pane's "Revert to derived" is the way back.
  auto surface_tool = std::make_unique<SurfaceTool>(document_, selection_);
  wire_status(surface_tool.get());
  properties_panel_->set_surface_tool(surface_tool.get());
  tool_manager_.register_tool(ToolId::Surface, std::move(surface_tool));

  // Terrain Brush: sculpt the height field (p5-s4, #234). Drag-paints a stroke
  // as ONE preview session + ONE command; its radius/strength/mode live in the
  // Tool Options row (build_tool_options_bar), so keep a pointer for them.
  auto terrain_brush_tool = std::make_unique<TerrainBrushTool>(document_);
  wire_status(terrain_brush_tool.get());
  terrain_brush_tool_ = terrain_brush_tool.get();
  tool_manager_.register_tool(ToolId::TerrainBrush, std::move(terrain_brush_tool));

  connect(actions_->tool_corner, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Corner);
  });
  connect(actions_->tool_stopline, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::StopLine);
  });
  connect(actions_->tool_junction_surface, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::JunctionSurface);
  });
  connect(actions_->tool_junction_span, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::JunctionSpan);
  });
  connect(actions_->tool_surface, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Surface);
  });
  connect(actions_->tool_terrain_brush, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::TerrainBrush);
  });
  connect(actions_->tool_maneuver, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Maneuver);
  });
  connect(actions_->tool_signal, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Signal);
    // Surface the 2D Editor pane so the Signal Phases timeline is reachable when
    // the signal workflow starts (mirrors the Elevation/Lane tools; the
    // discoverability rule — GW-4 step 4).
    editor2d_dock_->show();
    editor2d_dock_->raise();
    editor2d_host_->raise_relevant_page();
  });
  connect(actions_->tool_sign, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Sign);
  });
  tool_manager_.set_active(ToolId::Select);

  // Merge Roads: enabled only for exactly two selected roads mergeable in some
  // orientation; the trigger normalizes the argument order and pushes the merge
  // (reverse_road is deferred, so only End→Start merges).
  const auto update_merge_enabled = [this] {
    const std::vector<RoadId> roads = selection_.selected_roads();
    const bool ok = roads.size() == 2 &&
                    (edit::check_mergeable(document_.network(), roads[0], roads[1]).has_value() ||
                     edit::check_mergeable(document_.network(), roads[1], roads[0]).has_value());
    actions_->merge_roads->setEnabled(ok);
    // A greyed button with no explanation reads as broken — say why it is off.
    actions_->merge_roads->setToolTip(
        ok ? tr("Merge — join the two selected roads that meet end-to-start")
           : tr("Merge — select exactly two roads whose ends meet (one road's end at the "
                "other's start)"));
  };
  connect(&selection_, &SelectionModel::selection_changed, this, update_merge_enabled);
  connect(&document_,
          &Document::mesh_changed,
          this,
          [update_merge_enabled](const std::vector<RoadId>&) { update_merge_enabled(); });
  connect(actions_->merge_roads, &QAction::triggered, this, [this] {
    const std::vector<RoadId> roads = selection_.selected_roads();
    if (roads.size() != 2) {
      return;
    }
    RoadId a = roads[0];
    RoadId b = roads[1];
    if (!edit::check_mergeable(document_.network(), a, b).has_value()) {
      std::swap(a, b);
    }
    const Road* survivor = document_.network().road(a);
    const QString surviving =
        survivor != nullptr ? QString::fromStdString(survivor->odr_id) : QString();
    const Expected<void> merged =
        document_.push_command(edit::merge_roads(document_.network(), a, b));
    if (!merged.has_value()) {
      viewport_->show_toast(
          tr("Cannot merge: %1").arg(QString::fromStdString(merged.error().message)),
          ToastSeverity::Warning);
      return;
    }
    selection_.select({.road = a, .lane = LaneId{}}, SelectMode::Replace);
    viewport_->show_toast(tr("Merged into road %1 — Ctrl+Z to undo").arg(surviving),
                          ToastSeverity::Success);
  });

  // Terrain (p5-s2, #232): the create/remove pair toggle each other's enablement
  // on whether a field currently exists, refreshed whenever the mesh changes
  // (a create/remove/undo all re-emit mesh_changed).
  const auto update_terrain_enabled = [this] {
    const bool has_field = !document_.network().terrain().empty();
    actions_->terrain_create->setEnabled(!has_field);
    actions_->terrain_remove->setEnabled(has_field);
  };
  update_terrain_enabled();
  connect(&document_,
          &Document::mesh_changed,
          this,
          [update_terrain_enabled](const std::vector<RoadId>&) { update_terrain_enabled(); });
  connect(actions_->terrain_create, &QAction::triggered, this, [this] {
    const Expected<void> created =
        document_.push_command(edit::create_terrain_field(document_.network()));
    if (!created.has_value()) {
      viewport_->show_toast(
          tr("Cannot create terrain: %1").arg(QString::fromStdString(created.error().message)),
          ToastSeverity::Warning);
    } else {
      viewport_->show_toast(tr("Terrain field created — raise a road to shape it"),
                            ToastSeverity::Success);
    }
  });
  connect(actions_->terrain_remove, &QAction::triggered, this, [this] {
    const Expected<void> removed =
        document_.push_command(edit::remove_terrain_field(document_.network()));
    if (!removed.has_value()) {
      viewport_->show_toast(
          tr("Cannot remove terrain: %1").arg(QString::fromStdString(removed.error().message)),
          ToastSeverity::Warning);
    }
  });
  // DEM import (p5-s4, #234): read an ESRI ASCII grid and install it as the
  // scene field, as-is in the kernel frame (decision D1). The reader already
  // exists (p5-s2's sidecar); a malformed/unsafe grid warns and imports nothing.
  connect(actions_->terrain_import, &QAction::triggered, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import DEM (ESRI ASCII grid)"), QString(), tr("ESRI ASCII grid (*.asc)"));
    if (path.isEmpty()) {
      return;
    }
    Expected<roadmaker::TerrainParseResult> parsed =
        roadmaker::load_terrain_asc(std::filesystem::path(path.toStdString()));
    if (!parsed.has_value()) {
      viewport_->show_toast(
          tr("Cannot import DEM: %1").arg(QString::fromStdString(parsed.error().message)),
          ToastSeverity::Warning);
      return;
    }
    if (!parsed->diagnostics.empty()) {
      viewport_->show_toast(
          tr("Imported DEM with %n warning(s)", "", static_cast<int>(parsed->diagnostics.size())),
          ToastSeverity::Warning);
    }
    const int cols = static_cast<int>(parsed->field.cols);
    const int rows = static_cast<int>(parsed->field.rows);
    const Expected<void> installed = document_.push_command(
        edit::set_terrain_field(document_.network(), std::move(parsed->field)));
    if (!installed.has_value()) {
      viewport_->show_toast(
          tr("Cannot import DEM: %1").arg(QString::fromStdString(installed.error().message)),
          ToastSeverity::Warning);
      return;
    }
    viewport_->show_toast(tr("Imported DEM: %1×%2 posts").arg(cols).arg(rows),
                          ToastSeverity::Success);
  });
  // The Road Construction tool's automatic bridge assignment (p5-s3, #233):
  // detect grade-separated crossings and author a bridge over each. author_bridge
  // no-ops on a span already bridged, so re-running is safe.
  connect(actions_->bridge_generate, &QAction::triggered, this, [this] {
    const std::vector<roadmaker::GradeSeparation> crossings =
        roadmaker::find_grade_separations(document_.network());
    int built = 0;
    for (const roadmaker::GradeSeparation& sep : crossings) {
      constexpr double kAutoSpan = 24.0;
      const double s = std::max(0.0, sep.s_upper - (kAutoSpan / 2.0));
      if (document_.push_command(edit::author_bridge(document_.network(), sep.upper, s, kAutoSpan))
              .has_value()) {
        ++built;
      }
    }
    if (built > 0) {
      viewport_->show_toast(
          tr("Built %1 bridge structure(s) over grade-separated crossings").arg(built),
          ToastSeverity::Success);
    } else {
      viewport_->show_toast(tr("No un-bridged grade-separated crossings found"),
                            ToastSeverity::Info);
    }
  });
  // Passive detection hint: when a crossing appears that no bridge covers, nudge
  // the user toward the menu ONCE (the actionable one-click toast is a follow-up).
  // Reset when there are none, so a later crossing hints again.
  auto bridge_hint_shown = std::make_shared<bool>(false);
  connect(&document_,
          &Document::mesh_changed,
          this,
          [this, bridge_hint_shown](const std::vector<RoadId>&) {
            int unbridged = 0;
            for (const roadmaker::GradeSeparation& sep :
                 roadmaker::find_grade_separations(document_.network())) {
              const roadmaker::Road* road = document_.network().road(sep.upper);
              const bool covered =
                  road != nullptr &&
                  std::any_of(road->bridges.begin(), road->bridges.end(), [&](const auto& b) {
                    return b.s <= sep.s_upper && sep.s_upper <= b.s + b.length;
                  });
              if (!covered) {
                ++unbridged;
              }
            }
            if (unbridged > 0 && !*bridge_hint_shown) {
              viewport_->show_toast(
                  tr("Roads cross without a junction — Edit ▸ Bridge ▸ Generate Bridge Structures"),
                  ToastSeverity::Info);
              *bridge_hint_shown = true;
            } else if (unbridged == 0) {
              *bridge_hint_shown = false;
            }
          });

  // The freshly-built arrangement is the canonical layout Reset Layout
  // restores; user geometry (if any) is applied on top of it.
  default_layout_state_ = saveState();
  connect(actions_->reset_layout, &QAction::triggered, this, [this] {
    restoreState(default_layout_state_);
  });
  if (!restore_saved_layout || !settings_.restore_window(*this)) {
    // First run (no saved layout) or a scripted capture: keep the default
    // arrangement built above.
  }

  update_window_title();
  update_status_entities();
}

void MainWindow::build_docks() {
  scene_dock_ = new QDockWidget(tr("Scene"), this);
  scene_dock_->setObjectName(QStringLiteral("dock.scene"));
  scene_dock_->setWidget(new SceneTreePanel(scene_tree_model_, selection_, scene_dock_));
  scene_dock_->widget()->setMinimumWidth(260);
  addDockWidget(Qt::LeftDockWidgetArea, scene_dock_);

  // Library: the catalogue the user drags from (drop handler is P2.4). Loaded
  // from the bundled manifest; tabbed with the Scene tree on the left, Scene
  // raised by default.
  {
    QFile manifest(QStringLiteral(":/library/manifest.json"));
    if (manifest.open(QIODevice::ReadOnly)) {
      if (auto loaded = LibraryManifest::parse(manifest.readAll()); loaded.has_value()) {
        library_model_.set_manifest(std::move(*loaded));
      }
    }
  }
  library_dock_ = new QDockWidget(tr("Library"), this);
  library_dock_->setObjectName(QStringLiteral("dock.library"));
  library_panel_ = new LibraryPanel(library_model_, library_dock_);
  library_dock_->setWidget(library_panel_);
  addDockWidget(Qt::LeftDockWidgetArea, library_dock_);
  tabifyDockWidget(scene_dock_, library_dock_);
  scene_dock_->raise(); // Scene tree is the default front tab

  properties_dock_ = new QDockWidget(tr("Properties"), this);
  properties_dock_->setObjectName(QStringLiteral("dock.properties"));
  properties_panel_ = new PropertiesPanel(document_, selection_, properties_dock_);
  connect(properties_panel_, &PropertiesPanel::status_message, this, [this](const QString& text) {
    viewport_->show_toast(text);
  });
  // An engaged Attributes-pane slot sends the user to the Library: the panel
  // asks for a category, MainWindow owns the dock that can show it.
  connect(properties_panel_,
          &PropertiesPanel::library_category_requested,
          this,
          [this](const QString& category) {
            library_dock_->show();
            library_dock_->raise(); // it shares a tab stack with the Scene tree
            library_panel_->focus_category(category);
          });
  // Crosswalk asset authoring (p3-s2): the Library routes an asset selection to
  // the Attributes-pane editor; the editor's commits flow back through
  // MainWindow, which owns the project-overlay manifest and the propagation.
  properties_panel_->set_library_model(&library_model_);
  connect(library_panel_, &LibraryPanel::asset_selected, this, [this](const QString& key) {
    // A prop asset's Default scale is editable whenever a project is open — the
    // commit upserts a project-overlay entry that shadows the built-in (p6-s11).
    // Crosswalk/PropSet built-ins stay read-only (they need an explicit copy).
    const LibraryItem* item = library_model_.item_for_key(key);
    const bool is_prop = item != nullptr && item->kind == LibraryItem::Kind::Tree;
    const bool editable = project_.has_value() && (library_model_.has_overlay_item(key) || is_prop);
    properties_dock_->show();
    properties_dock_->raise();
    properties_panel_->edit_asset(key, editable);
  });
  // Library-first (#367): tracking the current asset arms its placement tool so
  // the toolbar mode and the Library selection stay in sync.
  connect(library_panel_,
          &LibraryPanel::asset_current_changed,
          this,
          &MainWindow::on_library_asset_current_changed);
  connect(library_panel_,
          &LibraryPanel::new_crosswalk_asset_requested,
          this,
          &MainWindow::create_crosswalk_asset);
  connect(properties_panel_,
          &PropertiesPanel::crosswalk_asset_committed,
          this,
          &MainWindow::commit_crosswalk_asset);
  connect(library_panel_,
          &LibraryPanel::new_prop_set_requested,
          this,
          &MainWindow::create_prop_set_asset);
  connect(properties_panel_,
          &PropertiesPanel::prop_set_asset_committed,
          this,
          &MainWindow::commit_prop_set_asset);
  connect(properties_panel_,
          &PropertiesPanel::prop_asset_committed,
          this,
          &MainWindow::commit_prop_asset);
  properties_dock_->setWidget(properties_panel_);
  properties_dock_->widget()->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, properties_dock_);

  // The 2D Editor pane hosts the flat, per-entity editors. The vertical
  // profile is the first page; the cross-section and Signal Phase Editor plug
  // in later (GW-4 step 4, p4-s5).
  //
  // NOT "dock.profile": restoreState() matches saved geometry by objectName, so
  // reusing the old name would drop this dock into the position a stale layout
  // remembers for a different widget. The new name is simply unknown to old
  // settings, which restoreState ignores — the default placement below wins.
  editor2d_dock_ = new QDockWidget(tr("2D Editor"), this);
  editor2d_dock_->setObjectName(QStringLiteral("dock.editor2d"));
  editor2d_host_ = new Editor2DHost(selection_, editor2d_dock_);
  editor2d_host_->register_page(
      std::make_unique<ProfileEditorPage>(document_, selection_, editor2d_host_));
  editor2d_host_->register_page(
      std::make_unique<WidthEditorPage>(document_, selection_, editor2d_host_));
  {
    auto phase_page =
        std::make_unique<SignalPhaseEditorPage>(document_, selection_, editor2d_host_);
    phase_page_ = phase_page.get();
    editor2d_host_->register_page(std::move(phase_page));
  }
  editor2d_dock_->setWidget(editor2d_host_);
  addDockWidget(Qt::BottomDockWidgetArea, editor2d_dock_);
  editor2d_dock_->hide(); // opt-in via the View menu — 2D editing is occasional

  // The phase panel drives a pane-independent viewport overlay (head colors,
  // moving-road highlight, dotted gate links) that works under ANY active tool.
  // PhasePanel stays viewport-unaware: it emits phase_view_changed, MainWindow
  // pulls its getters through the pure builder and pushes the result.
  connect(phase_page_->panel(), &PhasePanel::phase_view_changed, this, [this] {
    update_signal_phase_overlay();
  });
  connect(&document_, &Document::loaded, this, [this] { viewport_->clear_signal_phase_preview(); });
  connect(editor2d_dock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (!visible) {
      viewport_->clear_signal_phase_preview();
    } else {
      update_signal_phase_overlay();
    }
  });

  diagnostics_dock_ = new QDockWidget(tr("Diagnostics"), this);
  diagnostics_dock_->setObjectName(QStringLiteral("dock.diagnostics"));
  diagnostics_dock_->setWidget(
      new DiagnosticsPanel(document_, diagnostics_model_, selection_, diagnostics_dock_));
  addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock_);
  diagnostics_dock_->hide(); // collapsed by default until diagnostics arrive
}

void MainWindow::build_menus() {
  QMenu* file_menu = menuBar()->addMenu(tr("&File"));
  file_menu->addAction(actions_->new_file);
  file_menu->addAction(actions_->open);
  recent_menu_ = file_menu->addMenu(tr("Open &Recent"));
  update_recent_files_menu();
  file_menu->addSeparator();
  // Projects (p6-s1): plain directory pickers — a project is a folder with a
  // project.json, nothing heavier, so the flows stay two clicks.
  auto* new_project_action = new QAction(tr("New &Project…"), this);
  connect(new_project_action, &QAction::triggered, this, &MainWindow::new_project_dialog);
  file_menu->addAction(new_project_action);
  auto* open_project_action = new QAction(tr("Open Pro&ject…"), this);
  connect(open_project_action, &QAction::triggered, this, &MainWindow::open_project_dialog);
  file_menu->addAction(open_project_action);
  file_menu->addSeparator();
  file_menu->addAction(actions_->save);
  file_menu->addAction(actions_->save_as);
  file_menu->addSeparator();
  file_menu->addAction(actions_->export_glb);
#ifdef RM_HAVE_USD
  file_menu->addAction(actions_->export_usd);
#endif
  file_menu->addSeparator();
  auto* autosave_action = new QAction(tr("Enable &Autosave"), this);
  autosave_action->setCheckable(true);
  autosave_action->setChecked(settings_.autosave_enabled());
  connect(autosave_action, &QAction::toggled, this, [this](bool enabled) {
    settings_.set_autosave_enabled(enabled);
    autosave_.set_enabled(enabled);
  });
  file_menu->addAction(autosave_action);
  file_menu->addSeparator();
  file_menu->addAction(actions_->quit);

  QMenu* edit_menu = menuBar()->addMenu(tr("&Edit"));
  edit_menu->addAction(actions_->undo);
  edit_menu->addAction(actions_->redo);
  edit_menu->addSeparator();
  edit_menu->addAction(actions_->merge_roads);
  edit_menu->addAction(actions_->add_from_library);
  edit_menu->addSeparator();
  QMenu* terrain_menu = edit_menu->addMenu(tr("&Terrain"));
  terrain_menu->addAction(actions_->terrain_create);
  terrain_menu->addAction(actions_->terrain_remove);
  terrain_menu->addSeparator();
  terrain_menu->addAction(actions_->terrain_import);
  QMenu* bridge_menu = edit_menu->addMenu(tr("&Bridge"));
  bridge_menu->addAction(actions_->bridge_generate);

  QMenu* view_menu = menuBar()->addMenu(tr("&View"));
  view_menu->addAction(scene_dock_->toggleViewAction());
  view_menu->addAction(library_dock_->toggleViewAction());
  view_menu->addAction(properties_dock_->toggleViewAction());
  view_menu->addAction(diagnostics_dock_->toggleViewAction());
  view_menu->addAction(editor2d_dock_->toggleViewAction());
  view_menu->addSeparator();
  auto* textured_action = new QAction(tr("&Textured Rendering"), this);
  textured_action->setCheckable(true);
  textured_action->setChecked(settings_.textured_rendering());
  textured_action->setToolTip(tr("Daytime textured lighting vs the flat Sober look"));
  connect(textured_action, &QAction::toggled, this, [this](bool textured) {
    settings_.set_textured_rendering(textured);
    viewport_->set_textured_rendering(textured);
  });
  view_menu->addAction(textured_action);
  // Apply the persisted mode now (no-op when it matches the viewport default).
  viewport_->set_textured_rendering(settings_.textured_rendering());
  // The other viewport view-setting (#333): whether the active tool's hint is
  // drawn in the corner. Persisted the same way, defaults on, and never touches
  // the document — the status-bar instruction is a separate channel.
  actions_->viewport_hints->setChecked(settings_.viewport_hints());
  connect(actions_->viewport_hints, &QAction::toggled, this, [this](bool enabled) {
    settings_.set_viewport_hints(enabled);
    viewport_->set_hints_enabled(enabled);
  });
  view_menu->addAction(actions_->viewport_hints);
  viewport_->set_hints_enabled(settings_.viewport_hints());
  // Display units (#412, realism batch #411): metric ⇄ imperial at the widget
  // boundary only — files, commands and the kernel stay SI meters
  // (docs/domain/realism_defaults.md, Unit policy), so toggling never dirties
  // the document. Persisted like the other view settings.
  auto* imperial_action = new QAction(tr("&Imperial Units"), this);
  imperial_action->setCheckable(true);
  imperial_action->setChecked(settings_.display_units() == units::UnitSystem::Imperial);
  imperial_action->setToolTip(
      tr("Show and type lengths in feet instead of meters. Files stay metric."));
  connect(imperial_action, &QAction::toggled, this, [this](bool imperial) {
    const units::UnitSystem system =
        imperial ? units::UnitSystem::Imperial : units::UnitSystem::Metric;
    settings_.set_display_units(system);
    units::set_active(system);
  });
  view_menu->addAction(imperial_action);
  // The status bar holds formatted text, so it re-renders from the last hover
  // when the system flips instead of waiting for the next mouse move.
  connect(&units::Notifier::instance(), &units::Notifier::changed, this, [this] {
    on_hover(last_hover_);
  });
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_camera);
  view_menu->addAction(actions_->frame_selection);
  view_menu->addAction(actions_->frame_cursor);
  view_menu->addSeparator();
  // Every camera capability gets a visible, labelled entry point; the keys are
  // accelerators, not the only way in (product-parity discoverability rule).
  view_menu->addAction(actions_->view_perspective);
  view_menu->addAction(actions_->view_orthographic);
  QMenu* cardinal_menu = view_menu->addMenu(tr("&Cardinal Views"));
  cardinal_menu->addAction(actions_->view_north);
  cardinal_menu->addAction(actions_->view_south);
  cardinal_menu->addAction(actions_->view_west);
  cardinal_menu->addAction(actions_->view_east);
  cardinal_menu->addSeparator();
  cardinal_menu->addAction(actions_->view_top);
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_layout);

  QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
  help_menu->addAction(actions_->help_contents);
  // F1 / Help ▸ User Guide is context-sensitive: it opens the page for the
  // focused dock, else the active tool, else the guide index
  // (help::context_page, help_registry.hpp).
  connect(actions_->help_contents, &QAction::triggered, this, [this] {
    show_help(help::context_page(tool_manager_.active_id(), help_context_dock()));
  });
  QAction* tour_action = help_menu->addAction(tr("&Guided Tour"));
  tour_action->setToolTip(tr("Replay the 5-step first-run tour"));
  connect(tour_action, &QAction::triggered, this, &MainWindow::start_tour);
  help_menu->addSeparator();
  help_menu->addAction(actions_->about);
}

void MainWindow::build_toolbar() {
  // Two plain top-level QToolBars, both GENERATED from the action registry's
  // taxonomy (p1-s5/#317, flattened in #377) rather than hand-placed: a CORE
  // strip (File | Edit | Library & View) that never hides, and a single flat
  // tool row holding every placement tool at once (no category tabs). A tool
  // that lands without a toolbar_group fails shortcuts::toolbar_violations() in
  // the tests, so the taxonomy cannot silently go stale as later pillars add
  // tools.
  //
  // 28 px icons with the action's iconText under each (ui-design.md) — a new
  // user can read what every button does.
  const auto style_bar = [](QToolBar* bar, const QString& object_name) {
    bar->setObjectName(object_name);
    bar->setIconSize(QSize(28, 28));
    bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    bar->setMovable(false);
    // Give the overflow button a visible glyph. Qt's QToolBarExtension draws
    // its arrow through the native style, which our QSS `QToolBar QToolButton`
    // rule takes over — leaving a working but INVISIBLE button, so a narrow
    // window silently hides the overflowed group. An explicit icon is what
    // makes the overflow discoverable (verified at 920 px in the PR shots).
    if (auto* extension = bar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"))) {
      extension->setIcon(Icons::get(QStringLiteral("chevrons-right")));
      extension->setToolTip(tr("More tools"));
    }
    return bar;
  };

  const auto fill = [this](QToolBar* bar, shortcuts::ToolbarTab tab) {
    bool first_group = true;
    for (const shortcuts::ToolbarGroupLayout& group : shortcuts::toolbar_layout(tab)) {
      if (group.ids.empty()) {
        continue; // a reserved group renders nothing until its first tool lands
      }
      if (!first_group) {
        bar->addSeparator();
      }
      first_group = false;
      for (const shortcuts::Id id : group.ids) {
        QAction* action = actions_->action(id);
        Q_ASSERT(action != nullptr); // ActionMappingCoversEveryId gates this
        bar->addAction(action);
      }
    }
  };

  // The persistent CORE strip — File, the universal edit tools, and framing /
  // Library. It never hides behind a tab. Keeps the historical object name so a
  // saved layout still restores.
  core_toolbar_ = style_bar(addToolBar(tr("Main")), QStringLiteral("toolbar.main"));
  fill(core_toolbar_, shortcuts::ToolbarTab::kCore);

  all_toolbars_.push_back(core_toolbar_);

  // The SINGLE tool row — every placement / edit tool at once, grouped by
  // category with a separator between groups (Roads · Lanes · Markings · Props
  // · Signals …) and Qt's native overflow chevron when the window is narrow. It
  // is a plain top-level QToolBar in the same top area as the core strip, so the
  // two rows share one 8 px left origin and align by construction (#377). There
  // are no category tabs: showing every tool flat is the standard QToolBar idiom,
  // and the registry taxonomy still classifies each tool so toolbar_violations()
  // keeps gating drift as later pillars add tools.
  addToolBarBreak();
  tool_toolbar_ = style_bar(addToolBar(tr("Tools")), QStringLiteral("toolbar.tools"));
  bool first_tool_group = true;
  for (const shortcuts::ToolbarTabInfo& info : shortcuts::toolbar_tabs()) {
    for (const shortcuts::ToolbarGroupLayout& group : shortcuts::toolbar_layout(info.tab)) {
      if (group.ids.empty()) {
        continue; // a reserved group renders nothing until its first tool lands
      }
      if (!first_tool_group) {
        tool_toolbar_->addSeparator();
      }
      first_tool_group = false;
      for (const shortcuts::Id id : group.ids) {
        QAction* action = actions_->action(id);
        Q_ASSERT(action != nullptr); // ActionMappingCoversEveryId gates this
        tool_toolbar_->addAction(action);
      }
    }
  }
  all_toolbars_.push_back(tool_toolbar_);

  // addAction churn can drop the extension button's icon; (re-)apply the chevron
  // glyph so the overflow control stays visible when the row is too narrow.
  if (auto* extension =
          tool_toolbar_->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"))) {
    extension->setIcon(Icons::get(QStringLiteral("chevrons-right")));
    extension->setToolTip(tr("More tools"));
  }
}

void MainWindow::build_tool_options_bar() {
  // Contextual options row under the main toolbar. The Create Road template
  // choice lives here as a labeled dropdown (02 §2 moved it out of the icon
  // toolbar's hidden popup); other tools show their one-line usage hint.
  addToolBarBreak();
  options_bar_ = addToolBar(tr("Tool Options"));
  options_bar_->setObjectName(QStringLiteral("toolbar.options"));
  options_bar_->setIconSize(QSize(16, 16));
  options_bar_->setMovable(false);

  options_caption_ = new QLabel(tr("Template:"), options_bar_);
  options_caption_->setObjectName(QStringLiteral("toolOptionCaption"));
  options_caption_action_ = options_bar_->addWidget(options_caption_);

  template_button_ = new QToolButton(options_bar_);
  template_button_->setPopupMode(QToolButton::InstantPopup);
  template_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  template_button_->setToolTip(tr("Cross-section template the Create Road tool draws with"));
  auto* template_menu = new QMenu(template_button_);
  template_menu->addAction(actions_->template_rural);
  template_menu->addAction(actions_->template_urban);
  template_menu->addAction(actions_->template_highway);
  template_button_->setMenu(template_menu);
  const auto mirror_template = [this](const QAction* action) {
    template_button_->setIcon(action->icon());
    template_button_->setText(action->text().remove(QLatin1Char('&')));
  };
  mirror_template(actions_->template_group->checkedAction());
  connect(actions_->template_group,
          &QActionGroup::triggered,
          template_button_,
          [mirror_template](QAction* action) { mirror_template(action); });
  template_action_ = options_bar_->addWidget(template_button_);

  // Terrain Brush options (p5-s4, #234): mode/radius/strength. Fixed-width
  // controls added once and shown only while the brush is active (a variable
  // per-tool label here once shifted the row above — issue #332). They drive
  // the tool through its setters; the tool starts from these defaults.
  auto* mode_caption = new QLabel(tr("Brush:"), options_bar_);
  mode_caption->setObjectName(QStringLiteral("toolOptionCaption"));
  brush_option_actions_.push_back(options_bar_->addWidget(mode_caption));

  brush_mode_combo_ = new QComboBox(options_bar_);
  brush_mode_combo_->addItem(tr("Raise"), static_cast<int>(BrushMode::Raise));
  brush_mode_combo_->addItem(tr("Lower"), static_cast<int>(BrushMode::Lower));
  brush_mode_combo_->addItem(tr("Smooth"), static_cast<int>(BrushMode::Smooth));
  brush_mode_combo_->setToolTip(tr("What a brush stroke does to the ground"));
  brush_option_actions_.push_back(options_bar_->addWidget(brush_mode_combo_));

  auto* radius_caption = new QLabel(tr("Radius:"), options_bar_);
  radius_caption->setObjectName(QStringLiteral("toolOptionCaption"));
  brush_option_actions_.push_back(options_bar_->addWidget(radius_caption));
  brush_radius_spin_ = new UnitSpinBox(options_bar_);
  brush_radius_spin_->setRange(1.0, 500.0);
  brush_radius_spin_->setDecimals(0);
  brush_radius_spin_->setSingleStep(5.0);
  brush_radius_spin_->setValue(20.0);
  brush_radius_spin_->setToolTip(tr("Brush radius"));
  brush_option_actions_.push_back(options_bar_->addWidget(brush_radius_spin_));

  auto* strength_caption = new QLabel(tr("Strength:"), options_bar_);
  strength_caption->setObjectName(QStringLiteral("toolOptionCaption"));
  brush_option_actions_.push_back(options_bar_->addWidget(strength_caption));
  brush_strength_spin_ = new UnitSpinBox(options_bar_);
  brush_strength_spin_->setRange(0.01, 50.0);
  brush_strength_spin_->setDecimals(2);
  brush_strength_spin_->setSingleStep(0.1);
  brush_strength_spin_->setValue(0.5);
  brush_strength_spin_->setToolTip(tr("How hard one pass pushes (raise/lower) or blends (smooth)"));
  brush_option_actions_.push_back(options_bar_->addWidget(brush_strength_spin_));

  if (terrain_brush_tool_ != nullptr) {
    terrain_brush_tool_->set_radius(brush_radius_spin_->value());
    terrain_brush_tool_->set_strength(brush_strength_spin_->value());
    terrain_brush_tool_->set_mode(BrushMode::Raise);
    connect(brush_mode_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
      terrain_brush_tool_->set_mode(
          static_cast<BrushMode>(brush_mode_combo_->currentData().toInt()));
    });
    connect(brush_radius_spin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
      terrain_brush_tool_->set_radius(value);
    });
    connect(brush_strength_spin_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
      terrain_brush_tool_->set_strength(value);
    });
  }

  connect(&tool_manager_, &ToolManager::active_changed, this, &MainWindow::update_tool_options);
  update_tool_options();
}

void MainWindow::update_tool_options() {
  const QAction* active = actions_->tool_group->checkedAction();
  const bool create_road = active == actions_->tool_create_road;
  // Create Road gets its option control (labeled template dropdown); tools
  // without options leave the row empty rather than fill it with prose. A
  // tool-dependent label used to live here and its changing width shifted the
  // buttons of the row above on every tool switch (issue #332) — the per-tool
  // instruction belongs to the status bar and the viewport hint, both of which
  // sit clear of the toolbars.
  options_caption_action_->setVisible(create_road);
  template_action_->setVisible(create_road);

  const bool terrain_brush = active == actions_->tool_terrain_brush;
  for (QAction* option : brush_option_actions_) {
    option->setVisible(terrain_brush);
  }
}

void MainWindow::build_status_bar() {
  // Leftmost, with the stretch: the instruction is the bar's primary content
  // ("what can I do with this tool"), so it gets the stable left edge and the
  // slack. It is a NORMAL widget, so a transient showMessage() covers it for
  // its 5 s and Qt puts it back on clearMessage() — the instruction is still
  // true underneath, and the viewport corner hint carries it meanwhile.
  // Elided, because the sentence length varies per tool and the bar must not
  // resize with it (issue #332).
  status_instruction_ = new ElidedLabel(this);
  status_instruction_->setObjectName(QStringLiteral("status_instruction"));
  statusBar()->addWidget(status_instruction_, 1);
  statusBar()->addWidget(status_hover_);
  // Follow the active tool: its instruction() is the ONE source for "what does
  // this tool do", shown both here and as the viewport corner hint (issue #103
  // — during an interaction the user's eyes are on the viewport). Tools used to
  // emit the same sentence transiently on activate(); they no longer do, so the
  // transient channel carries only results and state-dependent guidance.
  const auto show_instruction = [this] {
    const Tool* tool = tool_manager_.active();
    const QString text = tool == nullptr ? QString() : tool->instruction();
    status_instruction_->set_full_text(text);
    viewport_->set_hint(text);
  };
  connect(&tool_manager_, &ToolManager::active_changed, this, show_instruction);
  show_instruction(); // the startup tool
  statusBar()->addPermanentWidget(status_entities_);
  auto* version_label =
      new QLabel(tr("kernel %1")
                     .arg(QString::fromUtf8(roadmaker::version().data(),
                                            static_cast<qsizetype>(roadmaker::version().size()))),
                 this);
  statusBar()->addPermanentWidget(version_label);
}

void MainWindow::load_file(const std::filesystem::path& path) {
  const auto result = document_.load(path);
  if (!result) {
    QMessageBox::warning(this,
                         tr("Load failed"),
                         tr("%1\n%2").arg(QString::fromStdString(result.error().message),
                                          QString::fromStdString(result.error().context)));
    return;
  }
  settings_.add_recent_file(document_.file_path());
  update_recent_files_menu();
  associate_project_for(path);
}

void MainWindow::associate_project_for(const std::filesystem::path& scene_path) {
  // Auto-associate the containing project (GW-2 step 1): a scene inside a
  // project directory adopts that project (Library overlay and all); a
  // standalone scene drops any project association.
  if (const auto project_dir = Project::find_project_for(scene_path)) {
    if (!project_.has_value() || project_->dir() != *project_dir) {
      if (auto opened = Project::open(*project_dir); opened.has_value()) {
        adopt_project(std::move(*opened));
      } else {
        clear_project();
      }
    }
  } else {
    clear_project();
  }
}

void MainWindow::new_project_dialog() {
  const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the project folder"));
  if (dir.isEmpty()) {
    return;
  }
  bool accepted = false;
  const QString name = QInputDialog::getText(this,
                                             tr("New Project"),
                                             tr("Project name:"),
                                             QLineEdit::Normal,
                                             QFileInfo(dir).fileName(),
                                             &accepted);
  if (!accepted || name.trimmed().isEmpty()) {
    return;
  }
  auto project = Project::create(std::filesystem::path(dir.toStdString()), name.trimmed());
  if (!project.has_value()) {
    QMessageBox::warning(this,
                         tr("New Project failed"),
                         tr("%1\n%2").arg(QString::fromStdString(project.error().message),
                                          QString::fromStdString(project.error().context)));
    return;
  }
  adopt_project(std::move(*project));
}

void MainWindow::open_project_dialog() {
  const QString dir = QFileDialog::getExistingDirectory(this, tr("Open project folder"));
  if (!dir.isEmpty()) {
    open_project_dir(dir);
  }
}

void MainWindow::open_project_dir(const QString& dir) {
  auto project = Project::open(std::filesystem::path(dir.toStdString()));
  if (!project.has_value()) {
    QMessageBox::warning(this,
                         tr("Open Project failed"),
                         tr("%1\n%2").arg(QString::fromStdString(project.error().message),
                                          QString::fromStdString(project.error().context)));
    return;
  }
  adopt_project(std::move(*project));
}

void MainWindow::adopt_project(Project project) {
  project_ = std::move(project);
  settings_.add_recent_project(QString::fromStdString(project_->dir().string()));
  apply_project_overlay();
  update_window_title();
  welcome_->set_active_project(QString::fromStdString(project_->dir().string()));
}

void MainWindow::clear_project() {
  if (!project_.has_value()) {
    return;
  }
  project_.reset();
  library_model_.clear_overlay(); // the overlay leaves with its project
  update_window_title();
  welcome_->set_active_project(QString());
}

void MainWindow::apply_project_overlay() {
  const auto manifest_path =
      project_.has_value() ? project_->library_manifest_path() : std::nullopt;
  if (!manifest_path.has_value()) {
    library_model_.clear_overlay();
    return;
  }
  auto manifest = LibraryManifest::load(*manifest_path);
  if (!manifest.has_value()) {
    // A broken overlay must not take the built-in catalogue down with it.
    spdlog::warn("project library overlay rejected: {} ({})",
                 manifest.error().message,
                 manifest.error().context);
    library_model_.clear_overlay();
    return;
  }
  // Overlay thumbnails are project-relative — resolve them against the project
  // directory (p6-s2) so a project's own PNGs load from disk.
  library_model_.set_overlay(std::move(*manifest),
                             QString::fromStdString(project_->dir().string()));
}

edit::CrosswalkParams MainWindow::resolve_default_crosswalk_params() const {
  // Prefer the asset the user picked in the Library (#367) so a Crosswalk tool
  // armed from a selection uses THAT crosswalk, not just the first one.
  if (const LibraryItem* cur = current_library_item();
      cur != nullptr && cur->kind == LibraryItem::Kind::Crosswalk) {
    return crosswalk_params_from_item(*cur, crosswalk_materials_);
  }
  // The first Kind::Crosswalk in the merged Library (an overlay asset shadows
  // the built-in); crosswalk.zebra is always present as the base fallback.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr && item->kind == LibraryItem::Kind::Crosswalk) {
      return crosswalk_params_from_item(*item, crosswalk_materials_);
    }
  }
  return {};
}

LibraryItem MainWindow::resolve_default_stencil_item() const {
  // Prefer the stencil the user picked in the Library (#367).
  if (const LibraryItem* cur = current_library_item();
      cur != nullptr && cur->kind == LibraryItem::Kind::Stencil) {
    return *cur;
  }
  // The first Kind::Stencil in the merged Library (an overlay asset shadows the
  // built-in). An empty item makes the Marking Point tool toast on click.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr && item->kind == LibraryItem::Kind::Stencil) {
      return *item;
    }
  }
  return {};
}

LibraryItem MainWindow::resolve_default_signal_item() const {
  // The first Kind::Signal in the merged Library (an overlay asset shadows the
  // built-in). An empty item makes the Signal tool toast rather than place.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr && item->kind == LibraryItem::Kind::Signal) {
      return *item;
    }
  }
  return {};
}

LibraryItem MainWindow::resolve_default_sign_item() const {
  // The Library's text-sign asset (signal tag "sign_text") — the Sign tool's
  // default. An empty item just makes SignTool fall back to the built-in text
  // plate, so the tool is always usable.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr && item->kind == LibraryItem::Kind::Signal &&
        item->signal == QStringLiteral("sign_text")) {
      return *item;
    }
  }
  return {};
}

LibraryItem MainWindow::resolve_default_prop_item() const {
  // Prefer the prop OR prop-set the user picked in the Library (#367) so a prop
  // tool armed from a selection scatters that asset. Both are valid prop assets
  // (is_prop_asset); the curve/span/polygon tools resolve a set per instance.
  if (const LibraryItem* cur = current_library_item();
      cur != nullptr &&
      (cur->kind == LibraryItem::Kind::Tree || cur->kind == LibraryItem::Kind::PropSet)) {
    LibraryItem resolved = *cur;
    // Fill each PropSet entry's transient Default scale from the merged library
    // so a scatter honors every drawn model's own default (resolve_prop_asset
    // carries the chosen entry's scale onto the synthetic tree). A Tree item has
    // no entries, so this is a no-op there.
    for (LibraryItem::PropSetEntry& entry : resolved.prop_entries) {
      entry.default_scale = library_model_.default_scale_for_model(entry.model);
    }
    return resolved;
  }
  // The first Kind::Tree in the merged Library (an overlay asset shadows the
  // built-in). An empty item makes the prop tools toast on the first click.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr && item->kind == LibraryItem::Kind::Tree) {
      return *item;
    }
  }
  return {};
}

LibraryItem MainWindow::resolve_default_marking_curve_item() const {
  // The first crosswalk OR plain marking asset in the merged Library — either
  // authors a marking curve. An empty item makes the tool toast on the first
  // click.
  for (int row = 0; row < library_model_.rowCount(); ++row) {
    const LibraryItem* item = library_model_.item(row);
    if (item != nullptr &&
        (item->kind == LibraryItem::Kind::Crosswalk || item->kind == LibraryItem::Kind::Marking)) {
      return *item;
    }
  }
  return {};
}

const LibraryItem* MainWindow::current_library_item() const {
  if (current_library_key_.isEmpty()) {
    return nullptr;
  }
  return library_model_.item_for_key(current_library_key_);
}

void MainWindow::on_library_asset_current_changed(const QString& key) {
  current_library_key_ = key;
  if (const LibraryItem* item = library_model_.item_for_key(key); item != nullptr) {
    arm_tool_for_library_item(*item);
  }
}

void MainWindow::arm_tool_for_library_item(const LibraryItem& item) {
  // Map a content asset to the tool that applies it. Only the fixed-default
  // placement tools have a Library counterpart; every other tool is a geometric
  // mode with no asset, so selecting those kinds leaves the active tool alone.
  QAction* tool = nullptr;
  switch (item.kind) {
  case LibraryItem::Kind::Tree:
    tool = actions_->tool_prop_point;
    break;
  case LibraryItem::Kind::PropSet:
    tool = actions_->tool_prop_curve; // a set scatters along a path
    break;
  case LibraryItem::Kind::Stencil:
    tool = actions_->tool_marking_point;
    break;
  case LibraryItem::Kind::Crosswalk:
    tool = actions_->tool_crosswalk;
    break;
  case LibraryItem::Kind::Signal:
    // The Sign tool authors text signs; other signals (traffic lights, stop /
    // yield) place through the Library drop or auto-signalize, not a single
    // placement tool, so they do not arm anything.
    if (item.signal == QStringLiteral("sign_text")) {
      tool = actions_->tool_sign;
    }
    break;
  default:
    break;
  }
  if (tool != nullptr && !tool->isChecked()) {
    tool->trigger();
    viewport_->show_toast(tr("%1 tool armed from Library").arg(tool->text()), ToastSeverity::Info);
  }
}

LibraryManifest MainWindow::load_or_create_overlay_manifest() const {
  if (project_.has_value()) {
    if (const auto path = project_->library_manifest_path()) {
      if (auto loaded = LibraryManifest::load(*path); loaded.has_value()) {
        return std::move(*loaded);
      }
    }
  }
  return {}; // a fresh manifest at the supported version
}

void MainWindow::create_crosswalk_asset() {
  if (!project_.has_value()) {
    viewport_->show_toast(tr("Open or create a project to author assets"), ToastSeverity::Info);
    return;
  }
  const auto path = project_->library_manifest_path();
  if (!path.has_value()) {
    return;
  }
  LibraryManifest manifest = load_or_create_overlay_manifest();
  // A unique key across base + overlay so the new asset never shadows a built-in.
  QString key;
  for (int n = 1;; ++n) {
    key = QStringLiteral("crosswalk.custom%1").arg(n);
    if (library_model_.item_for_key(key) == nullptr) {
      break;
    }
  }
  LibraryItem item;
  item.key = key;
  item.label = tr("Crosswalk %1").arg(key.mid(QStringLiteral("crosswalk.custom").size()));
  item.category = QStringLiteral("Crosswalks");
  item.kind = LibraryItem::Kind::Crosswalk;
  item.crosswalk_material = QStringLiteral("material.paint_white");
  item.crosswalk_segmentation = QStringLiteral("crosswalk");
  manifest.upsert(item);
  if (!manifest.save(*path).has_value()) {
    viewport_->show_toast(tr("Couldn't write the project library"), ToastSeverity::Warning);
    return;
  }
  apply_project_overlay(); // reload the overlay so the model sees the new asset
  library_dock_->show();
  library_dock_->raise();
  library_panel_->select_asset(key); // selects it and opens its editor
}

void MainWindow::commit_crosswalk_asset(const LibraryItem& item) {
  if (!project_.has_value()) {
    return;
  }
  const auto path = project_->library_manifest_path();
  if (!path.has_value()) {
    return;
  }
  LibraryManifest manifest = load_or_create_overlay_manifest();
  manifest.upsert(item);
  if (!manifest.save(*path).has_value()) {
    viewport_->show_toast(tr("Couldn't write the project library"), ToastSeverity::Warning);
    return;
  }
  apply_project_overlay(); // refresh the Library preview/icons for the edited asset
  // Propagate the asset change to every following instance in one undoable
  // command (the manifest write itself is not undoable — documented follow-up).
  if (auto command = propagate_crosswalk_asset(document_.network(),
                                               item,
                                               crosswalk_materials_,
                                               tr("Edit crosswalk asset").toStdString())) {
    (void)document_.push_command(std::move(command));
  }
}

void MainWindow::create_prop_set_asset() {
  if (!project_.has_value()) {
    viewport_->show_toast(tr("Open or create a project to author assets"), ToastSeverity::Info);
    return;
  }
  const auto path = project_->library_manifest_path();
  if (!path.has_value()) {
    return;
  }
  LibraryManifest manifest = load_or_create_overlay_manifest();
  // A unique key across base + overlay so the new set never shadows a built-in.
  QString key;
  for (int n = 1;; ++n) {
    key = QStringLiteral("prop_set.custom%1").arg(n);
    if (library_model_.item_for_key(key) == nullptr) {
      break;
    }
  }
  LibraryItem item;
  item.key = key;
  item.label = tr("Prop set %1").arg(key.mid(QStringLiteral("prop_set.custom").size()));
  item.category = QStringLiteral("Prop sets");
  item.kind = LibraryItem::Kind::PropSet;
  // Default mixed set (real bundled ids): mostly pines with a few shrubs.
  item.prop_entries.push_back(
      LibraryItem::PropSetEntry{.model = QStringLiteral("tree_pine"), .portion = 3.0});
  item.prop_entries.push_back(
      LibraryItem::PropSetEntry{.model = QStringLiteral("shrub"), .portion = 1.0});
  manifest.upsert(item);
  if (!manifest.save(*path).has_value()) {
    viewport_->show_toast(tr("Couldn't write the project library"), ToastSeverity::Warning);
    return;
  }
  apply_project_overlay(); // reload the overlay so the model sees the new asset
  library_dock_->show();
  library_dock_->raise();
  library_panel_->select_asset(key); // selects it and opens its editor
}

void MainWindow::commit_prop_set_asset(const LibraryItem& item) {
  if (!project_.has_value()) {
    return;
  }
  const auto path = project_->library_manifest_path();
  if (!path.has_value()) {
    return;
  }
  LibraryManifest manifest = load_or_create_overlay_manifest();
  manifest.upsert(item);
  if (!manifest.save(*path).has_value()) {
    viewport_->show_toast(tr("Couldn't write the project library"), ToastSeverity::Warning);
    return;
  }
  // No propagation: a scatter bakes independent prop objects that never
  // reference the set, so editing the set only changes future scatters (unlike
  // commit_crosswalk_asset, whose instances carry the asset's geometry).
  apply_project_overlay(); // refresh the Library entry for the edited set
}

void MainWindow::commit_prop_asset(const LibraryItem& item) {
  if (!project_.has_value()) {
    return;
  }
  const auto path = project_->library_manifest_path();
  if (!path.has_value()) {
    return;
  }
  LibraryManifest manifest = load_or_create_overlay_manifest();
  manifest.upsert(item); // shadows the built-in prop with a project-owned copy
  if (!manifest.save(*path).has_value()) {
    viewport_->show_toast(tr("Couldn't write the project library"), ToastSeverity::Warning);
    return;
  }
  // No propagation: Default scale affects new placements only (each placed prop
  // already bakes its absolute @height/@radius and never references the asset),
  // same rationale as commit_prop_set_asset.
  apply_project_overlay(); // refresh the Library entry for the edited prop
}

void MainWindow::set_capture_highlights(const QString& select_odr, const QString& hover_odr) {
  if (!select_odr.isEmpty()) {
    const RoadId road = document_.network().find_road(select_odr.toStdString());
    if (road.is_valid()) {
      selection_.select({.road = road, .lane = LaneId{}}, SelectMode::Replace);
    }
  }
  if (!hover_odr.isEmpty()) {
    const RoadId road = document_.network().find_road(hover_odr.toStdString());
    if (road.is_valid()) {
      viewport_->set_hover_preview(road);
    }
  }
}

void MainWindow::activate_tool_for_capture(const QString& tool_id) {
  static const std::map<QString, ToolId> kTools{
      {QStringLiteral("select"), ToolId::Select},
      {QStringLiteral("move"), ToolId::Move},
      {QStringLiteral("create-road"), ToolId::CreateRoad},
      {QStringLiteral("edit-nodes"), ToolId::EditNodes},
      {QStringLiteral("lane-profile"), ToolId::LaneProfile},
      {QStringLiteral("elevation"), ToolId::Elevation},
      {QStringLiteral("create-junction"), ToolId::CreateJunction},
      {QStringLiteral("split"), ToolId::Split},
      {QStringLiteral("delete"), ToolId::Delete},
      {QStringLiteral("lane-add"), ToolId::LaneAdd},
      {QStringLiteral("lane-form"), ToolId::LaneForm},
      {QStringLiteral("lane-carve"), ToolId::LaneCarve},
      {QStringLiteral("crosswalk"), ToolId::Crosswalk},
      {QStringLiteral("markingPoint"), ToolId::MarkingPoint},
      {QStringLiteral("markingCurve"), ToolId::MarkingCurve},
      {QStringLiteral("propPoint"), ToolId::PropPoint},
      {QStringLiteral("propCurve"), ToolId::PropCurve},
      {QStringLiteral("propSpan"), ToolId::PropSpan},
      {QStringLiteral("propPolygon"), ToolId::PropPolygon},
      {QStringLiteral("corner"), ToolId::Corner},
      {QStringLiteral("stopline"), ToolId::StopLine},
      {QStringLiteral("junctionSpan"), ToolId::JunctionSpan},
  };
  if (const auto found = kTools.find(tool_id); found != kTools.end()) {
    tool_manager_.set_active(found->second);
  }
}

void MainWindow::raise_dock_for_capture(const QString& object_name) {
  for (QDockWidget* dock : findChildren<QDockWidget*>()) {
    if (dock->objectName() == object_name) {
      // Docks that are opt-in (hidden by default, e.g. the 2D Editor) must be
      // shown before raise() can bring them to the front of a capture.
      dock->show();
      dock->raise();
      return;
    }
  }
}

void MainWindow::drop_library_item_for_capture(const QString& key, double world_x, double world_y) {
  on_library_drop(key, world_x, world_y);
}

void MainWindow::preview_library_drag_for_capture(const QString& key,
                                                  double world_x,
                                                  double world_y) {
  on_library_drag_moved(key, world_x, world_y);
}

void MainWindow::on_library_drop(const QString& key, double world_x, double world_y) {
  const LibraryItem* item = library_model_.item_for_key(key);
  if (item == nullptr) {
    return;
  }
  LibraryDropAction action = resolve_library_drop(*item, document_.network(), world_x, world_y);
  switch (action.kind) {
  case LibraryDropKind::RoadTemplate:
    actions_->tool_create_road->trigger(); // activate Create Road
    if (create_road_tool_ != nullptr) {
      create_road_tool_->set_profile(action.profile);
      create_road_tool_->begin_at(world_x, world_y);
    }
    viewport_->show_toast(tr("Create Road armed — click to add points"), ToastSeverity::Info);
    break;
  case LibraryDropKind::PropSet:
    // A dropped prop set arms Prop Curve with the set current (#367): track the
    // key so the tool's resolver returns this set, then activate the tool.
    current_library_key_ = key;
    actions_->tool_prop_curve->trigger();
    viewport_->show_toast(action.toast, ToastSeverity::Info);
    viewport_->clear_drag_target_road();
    break;
  case LibraryDropKind::RoadStyle:
  case LibraryDropKind::Assembly:
  case LibraryDropKind::Tree:
  case LibraryDropKind::Signal:
  case LibraryDropKind::Marking:
  case LibraryDropKind::Material:
  case LibraryDropKind::Stencil:
    if (document_.push_command(std::move(action.command)).has_value()) {
      viewport_->show_toast(action.toast, ToastSeverity::Success);
    } else {
      viewport_->show_toast(tr("Couldn't place that here"), ToastSeverity::Warning);
    }
    viewport_->clear_drag_target_road();
    break;
  case LibraryDropKind::Crosswalk:
    // The crosswalk object and its stop-line link land as ONE undo unit, so a
    // single Ctrl+Z removes the whole drop.
    document_.undo_stack()->beginMacro(tr("Place crosswalk"));
    for (auto& [road, object] : action.objects) {
      (void)document_.push_command(edit::add_object(document_.network(), road, std::move(object)));
    }
    if (action.stopline_link.has_value()) {
      (void)document_.push_command(
          edit::set_stopline_distance(document_.network(),
                                      action.stopline_link->junction,
                                      action.stopline_link->arm,
                                      kStopLineDefaultDistance,
                                      action.stopline_link->crosswalk_odr_id));
    }
    document_.undo_stack()->endMacro();
    viewport_->show_toast(action.toast, ToastSeverity::Success);
    viewport_->clear_drag_target_road();
    break;
  case LibraryDropKind::None:
    // A resolver may reject with a hint (e.g. a tree dropped away from any road).
    if (!action.toast.isEmpty()) {
      viewport_->show_toast(action.toast, ToastSeverity::Info);
    }
    viewport_->clear_drag_target_road();
    break;
  }
}

void MainWindow::on_library_drag_moved(const QString& key, double world_x, double world_y) {
  const LibraryItem* item = library_model_.item_for_key(key);
  if (item == nullptr) {
    viewport_->clear_drop_preview();
    return;
  }
  // Resolve through the SAME path the drop commit uses, so the ghost marks
  // exactly where the element lands (ghost==commit). The command is discarded;
  // only the resolved landing point drives the preview.
  const LibraryDropAction action =
      resolve_library_drop(*item, document_.network(), world_x, world_y);
  viewport_->set_drop_preview(action.preview.x, action.preview.y, action.preview.valid);
  // A road-style drag highlights the road it would apply to (ghost==target);
  // every other item kind clears any lingering highlight.
  if (action.kind == LibraryDropKind::RoadStyle) {
    viewport_->set_drag_target_road(action.target_road);
  } else {
    viewport_->clear_drag_target_road();
  }
}

void MainWindow::start_tour() {
  if (tour_overlay_ == nullptr) {
    tour_overlay_ = new TourOverlay(default_tour_steps(), this);
    // Resolve a step's target (an action iconText) to its toolbar button rect.
    // Every tool now lives on a permanently-shown bar (the core strip or the
    // single flat tool row, #377), so a straight scan of all_toolbars_ finds it.
    tour_overlay_->set_target_resolver([this](const QString& key) -> QRect {
      const auto find_on_shown_bars = [this, &key]() -> QRect {
        for (QToolBar* bar : all_toolbars_) {
          if (bar == nullptr) {
            continue;
          }
          for (QAction* action : bar->actions()) {
            if (action->iconText() != key) {
              continue;
            }
            QWidget* widget = bar->widgetForAction(action);
            if (widget != nullptr && widget->isVisible()) {
              const QPoint top_left =
                  tour_overlay_->mapFromGlobal(widget->mapToGlobal(QPoint(0, 0)));
              return {top_left, widget->size()};
            }
          }
        }
        return {};
      };
      return find_on_shown_bars();
    });
    connect(tour_overlay_, &TourOverlay::finished, this, [this] {
      settings_.set_tour_seen(true);
      if (tour_overlay_ != nullptr) {
        tour_overlay_->hide();
      }
    });
  }
  tour_overlay_->setGeometry(rect());
  tour_overlay_->begin();
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  if (tour_checked_ || !allow_first_run_tour_) {
    return;
  }
  tour_checked_ = true;
  if (settings_.tour_seen()) {
    return;
  }
  // Defer one loop turn so the toolbar has its final geometry to highlight.
  QTimer::singleShot(0, this, [this] { start_tour(); });
}

void MainWindow::new_file() {
  if (!confirm_discard()) {
    return;
  }
  document_.reset();
}

bool MainWindow::save_file() {
  if (!document_.has_file()) {
    return save_file_as();
  }
  return save_to(std::filesystem::path(document_.file_path().toStdString()));
}

bool MainWindow::save_file_as() {
  // An unsaved scene defaults into the active project's directory, so "New
  // Scene in Project" lands where the project globs its scenes from.
  const QString untitled =
      project_.has_value() ? QString::fromStdString((project_->dir() / "untitled.xodr").string())
                           : QStringLiteral("untitled.xodr");
  const QString suggested = document_.has_file() ? document_.file_path() : untitled;
  // QFileDialog owns the overwrite prompt (§8 edge cases).
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save OpenDRIVE file"), suggested, tr("OpenDRIVE (*.xodr);;All files (*)"));
  if (path.isEmpty()) {
    return false;
  }
  return save_to(std::filesystem::path(path.toStdString()));
}

bool MainWindow::save_to(const std::filesystem::path& path) {
  const auto result = document_.save(path);
  // Validator findings (rule ids included) landed in the Diagnostics panel
  // either way — errors never block the save, but the user sees what a
  // consumer would (§8).
  diagnostics_dock_->setVisible(!document_.diagnostics().empty() || diagnostics_dock_->isVisible());
  if (!result) {
    QMessageBox::warning(this,
                         tr("Save failed"),
                         tr("%1\n%2").arg(QString::fromStdString(result.error().message),
                                          QString::fromStdString(result.error().context)));
    return false;
  }
  settings_.add_recent_file(document_.file_path());
  update_recent_files_menu();
  associate_project_for(path); // Save As into a project folder joins it
  save_welcome_thumbnail();
  viewport_->show_toast(tr("Saved %1").arg(document_.file_path()), ToastSeverity::Success);
  return true;
}

void MainWindow::save_welcome_thumbnail() {
  const QString thumb_path = WelcomeWidget::thumbnail_path_for(document_.file_path());
  if (thumb_path.isEmpty()) {
    return;
  }
  const QImage frame = viewport_->capture_frame();
  if (frame.isNull()) {
    return; // GL-less session (tests) — the welcome tile keeps a placeholder
  }
  frame.scaled(440, 248, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).save(thumb_path);
}

bool MainWindow::confirm_discard() {
  if (!document_.is_dirty()) {
    return true;
  }
  const auto choice =
      QMessageBox::warning(this,
                           tr("Unsaved changes"),
                           tr("The document has unsaved changes. Save them first?"),
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                           QMessageBox::Save);
  if (choice == QMessageBox::Save) {
    return save_file();
  }
  return choice == QMessageBox::Discard;
}

void MainWindow::open_file_dialog() {
  // With a project open, browsing starts among its scenes.
  const QString start_dir =
      project_.has_value() ? QString::fromStdString(project_->dir().string()) : QString();
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open OpenDRIVE file"), start_dir, tr("OpenDRIVE (*.xodr);;All files (*)"));
  if (!path.isEmpty()) {
    load_file(std::filesystem::path(path.toStdString()));
  }
}

void MainWindow::export_file_dialog() {
  const QString suggested = document_.has_file()
                                ? QFileInfo(document_.file_path()).completeBaseName() + ".glb"
                                : QStringLiteral("network.glb");
  const QString path =
      QFileDialog::getSaveFileName(this, tr("Export glTF"), suggested, tr("Binary glTF (*.glb)"));
  if (path.isEmpty()) {
    return;
  }
  const auto result = document_.export_glb(std::filesystem::path(path.toStdString()));
  if (!result) {
    QMessageBox::warning(this, tr("Export failed"), QString::fromStdString(result.error().message));
  } else {
    viewport_->show_toast(tr("Exported %1").arg(path), ToastSeverity::Success);
  }
}

#ifdef RM_HAVE_USD
void MainWindow::export_usd_dialog() {
  const QString suggested = document_.has_file()
                                ? QFileInfo(document_.file_path()).completeBaseName() + ".usda"
                                : QStringLiteral("network.usda");
  // USDA (ASCII) only — .usdc/.usdz crate output is not supported in M2
  // (docs/design/m2/04_usd_export.md).
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Export USD (ASCII .usda only)"), suggested, tr("OpenUSD ASCII (*.usda)"));
  if (path.isEmpty()) {
    return;
  }
  const auto result = document_.export_usd(std::filesystem::path(path.toStdString()));
  if (!result) {
    QMessageBox::warning(this, tr("Export failed"), QString::fromStdString(result.error().message));
  } else {
    viewport_->show_toast(tr("Exported %1").arg(path), ToastSeverity::Success);
  }
}
#endif

void MainWindow::check_crash_reports() {
  const std::vector<crash::PendingReport> reports =
      crash::pending_reports(crash::default_report_dir(), crash::current_session());
  if (reports.empty()) {
    return;
  }
  // The report is half as useful without the command trail — pull the
  // crashed session's log tail in before the user files it anywhere.
  const crash::PendingReport& newest = reports.front();
  crash::append_log_tail(newest.path,
                         logging::log_file_for(logging::default_log_dir(), newest.session));

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Previous session crashed"));
  box.setText(tr("The previous RoadMaker session ended in a crash."));
  box.setInformativeText(tr("A crash report was saved to:\n%1\n\n"
                            "Reports stay on this machine — nothing is uploaded or sent "
                            "anywhere. If you can, please attach the report to a GitHub issue "
                            "(the \"crash\" template) so the crash gets fixed.")
                             .arg(QString::fromStdString(newest.path.string())));
  QPushButton* open_button = box.addButton(tr("Open Folder"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Close);
  box.exec();
  if (box.clickedButton() == open_button) {
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(newest.path.parent_path().string())));
  }
  // Acknowledge them all — each report is offered exactly once; the files
  // stay on disk for the user.
  for (const crash::PendingReport& report : reports) {
    crash::acknowledge(report);
  }
}

void MainWindow::check_recovery() {
  const std::vector<RecoverySet> sets = AutosaveManager::pending_recoveries(
      AutosaveManager::default_recovery_dir(), autosave_.session());
  const RecoverySet* candidate = nullptr;
  for (const RecoverySet& set : sets) {
    if (!AutosaveManager::should_recover(set)) {
      // Stale clean leftovers — nothing to recover, sweep them.
      AutosaveManager::discard(set);
    } else if (candidate == nullptr) {
      // Offer only the newest set this startup; older crashed sessions stay
      // on disk and are offered on the next start.
      candidate = &set;
    }
  }
  if (candidate == nullptr) {
    return;
  }
  const QString when = QLocale().toString(QDateTime::fromMSecsSinceEpoch(candidate->written_ms),
                                          QLocale::ShortFormat);
  const QString name = candidate->original_path.isEmpty()
                           ? tr("an unsaved document")
                           : QFileInfo(candidate->original_path).fileName();
  const auto choice = QMessageBox::question(this,
                                            tr("Recover unsaved work"),
                                            tr("RoadMaker did not shut down cleanly.\n"
                                               "Recover unsaved work on %1 from %2?")
                                                .arg(name, when),
                                            QMessageBox::Yes | QMessageBox::No,
                                            QMessageBox::Yes);
  if (choice != QMessageBox::Yes) {
    AutosaveManager::discard(*candidate);
    return;
  }
  const auto result = document_.load(candidate->xodr);
  if (!result) {
    // Keep the set on disk — the user can retry on the next start.
    QMessageBox::warning(this,
                         tr("Recovery failed"),
                         tr("%1\n%2").arg(QString::fromStdString(result.error().message),
                                          QString::fromStdString(result.error().context)));
    return;
  }
  document_.mark_recovered(candidate->original_path);
  AutosaveManager::discard(*candidate);
  // Re-protect the recovered (dirty) document under this session right away.
  (void)autosave_.autosave_now();
  update_window_title();
}

void MainWindow::show_about_dialog() {
  QMessageBox about(this);
  about.setWindowTitle(tr("About RoadMaker"));
  about.setTextFormat(Qt::RichText);
  about.setIconPixmap(Icons::app_icon().pixmap(64, 64));
  about.setText(tr("<b>RoadMaker</b> — open source road-network authoring toolkit.<br>"
                   "Kernel %1<br><br>"
                   "Licensed under the Apache License 2.0 — © Robomous.<br><br>"
                   "Built with Qt %2, used under the GNU LGPLv3 as dynamically linked "
                   "libraries. You may replace the bundled Qt libraries with your own "
                   "builds; see THIRD_PARTY_LICENSES.md in the distribution.")
                    .arg(QString::fromUtf8(roadmaker::version().data(),
                                           static_cast<qsizetype>(roadmaker::version().size())),
                         QStringLiteral(QT_VERSION_STR)));
  about.exec();
}

void MainWindow::show_help(const QString& slug) {
  if (help_viewer_.isNull()) {
    // Top-level window (no parent) so it lives beside the editor, not inside it.
    help_viewer_ = new help::HelpViewer(nullptr);
    help_viewer_->setAttribute(Qt::WA_DeleteOnClose);
  }
  help_viewer_->open_page(slug);
  help_viewer_->show();
  help_viewer_->raise();
  help_viewer_->activateWindow();
}

QString MainWindow::help_context_dock() const {
  // Walk up from the focused widget to the enclosing QDockWidget. The dock's
  // objectName is the key help::page_for_dock maps; an empty string (focus in
  // the viewport, a toolbar, or nowhere) makes context help fall through to the
  // active tool.
  for (const QWidget* w = QApplication::focusWidget(); w != nullptr; w = w->parentWidget()) {
    if (const auto* dock = qobject_cast<const QDockWidget*>(w)) {
      return dock->objectName();
    }
  }
  return {};
}

void MainWindow::update_recent_files_menu() {
  recent_menu_->clear();
  const QStringList recent = settings_.recent_files();
  recent_menu_->setEnabled(!recent.isEmpty());
  for (const QString& path : recent) {
    QAction* action = recent_menu_->addAction(path);
    connect(action, &QAction::triggered, this, [this, path] {
      load_file(std::filesystem::path(path.toStdString()));
    });
  }
}

void MainWindow::update_window_title() {
  // [*] renders the QUndoStack dirty flag (setWindowModified).
  const QString name =
      document_.has_file() ? QFileInfo(document_.file_path()).fileName() : tr("Untitled");
  if (project_.has_value()) {
    setWindowTitle(tr("%1[*] — %2 — RoadMaker").arg(name, project_->name()));
  } else {
    setWindowTitle(tr("%1[*] — RoadMaker").arg(name));
  }
}

void MainWindow::update_status_entities() {
  const RoadNetwork& network = document_.network();
  status_entities_->setText(tr("%1 roads · %2 lanes · %3 junctions")
                                .arg(network.road_count())
                                .arg(network.lane_count())
                                .arg(network.junction_count()));
}

void MainWindow::update_signal_phase_overlay() {
  PhasePanel* panel = phase_page_ != nullptr ? phase_page_->panel() : nullptr;
  if (panel == nullptr || !editor2d_dock_->isVisible() || !panel->junction().is_valid()) {
    viewport_->clear_signal_phase_preview();
    return;
  }
  viewport_->set_signal_phase_preview(build_signal_phase_preview(document_.network(),
                                                                 panel->junction(),
                                                                 panel->signal_states_at_playhead(),
                                                                 panel->moving_roads()));
}

void MainWindow::on_hover(const HoverInfo& info) {
  last_hover_ = info;
  if (!info.valid) {
    status_hover_->clear();
    return;
  }
  QString text = tr("x %1 · y %2")
                     .arg(units::format_length(info.world_x, 2))
                     .arg(units::format_length(info.world_y, 2));
  if (info.on_road) {
    text += tr("  ·  %1  ·  s %2 · t %3")
                .arg(info.entity)
                .arg(units::format_length(info.s, 2))
                .arg(units::format_length(info.t, 2));
  }
  status_hover_->setText(text);
}

void MainWindow::changeEvent(QEvent* event) {
  // The tinted icon cache bakes in the palette's WindowText color — rebuild
  // it when the theme flips (e.g. macOS light/dark) so icons follow.
  if (event->type() == QEvent::ApplicationPaletteChange) {
    Icons::clear_cache();
    actions_->apply_icons();
  }
  QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!confirm_discard()) {
    event->ignore();
    return;
  }
  // Explicit close: whatever the user decided in confirm_discard, the
  // recovery set is moot (§3 cleanup rule).
  autosave_.clear_recovery();
  settings_.save_window(*this);
  QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  const QMimeData* mime = event->mimeData();
  if (mime->hasUrls() && !mime->urls().isEmpty() &&
      mime->urls().first().toLocalFile().endsWith(QStringLiteral(".xodr"), Qt::CaseInsensitive)) {
    event->acceptProposedAction();
  }
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (!urls.isEmpty()) {
    load_file(std::filesystem::path(urls.first().toLocalFile().toStdString()));
    event->acceptProposedAction();
  }
}

} // namespace roadmaker::editor
