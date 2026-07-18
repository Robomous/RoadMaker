#include "app/main_window.hpp"

#include "roadmaker/version.hpp"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
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
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>

#include "app/context_menu.hpp"
#include "app/crash_handler.hpp"
#include "app/icons.hpp"
#include "app/log_setup.hpp"
#include "app/tour_controller.hpp"
#include "app/tour_overlay.hpp"
#include "document/crosswalk_item.hpp"
#include "document/library_drop.hpp"
#include "document/library_manifest.hpp"
#include "help/help_registry.hpp"
#include "help/help_viewer.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/editor2d_host.hpp"
#include "panels/library_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"
#include "tools/create_junction_tool.hpp"
#include "tools/create_road_tool.hpp"
#include "tools/delete_tool.hpp"
#include "tools/edit_nodes_tool.hpp"
#include "tools/elevation_tool.hpp"
#include "tools/lane_add_tool.hpp"
#include "tools/lane_carve_tool.hpp"
#include "tools/lane_form_tool.hpp"
#include "tools/lane_profile_tool.hpp"
#include "tools/select_tool.hpp"
#include "tools/split_tool.hpp"

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
    const bool editable = project_.has_value() && library_model_.has_overlay_item(key);
    properties_dock_->show();
    properties_dock_->raise();
    properties_panel_->edit_asset(key, editable);
  });
  connect(library_panel_,
          &LibraryPanel::new_crosswalk_asset_requested,
          this,
          &MainWindow::create_crosswalk_asset);
  connect(properties_panel_,
          &PropertiesPanel::crosswalk_asset_committed,
          this,
          &MainWindow::commit_crosswalk_asset);
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
  editor2d_dock_->setWidget(editor2d_host_);
  addDockWidget(Qt::BottomDockWidgetArea, editor2d_dock_);
  editor2d_dock_->hide(); // opt-in via the View menu — 2D editing is occasional

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
  // Labeled toolbar (ui-design.md): 28 px icons with the action's iconText
  // under each, grouped File | Tools | View — a new user can read what
  // every button does.
  QToolBar* toolbar = addToolBar(tr("Main"));
  main_toolbar_ = toolbar; // the guided tour highlights buttons via this
  toolbar->setObjectName(QStringLiteral("toolbar.main"));
  toolbar->setIconSize(QSize(28, 28));
  toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  toolbar->setMovable(false);
  toolbar->addAction(actions_->new_file);
  toolbar->addAction(actions_->open);
  toolbar->addAction(actions_->save);
  toolbar->addAction(actions_->export_glb);
  toolbar->addSeparator();
  toolbar->addAction(actions_->tool_select);
  toolbar->addAction(actions_->tool_move);
  toolbar->addAction(actions_->tool_create_road);
  toolbar->addAction(actions_->tool_edit_nodes);
  toolbar->addAction(actions_->tool_lane_profile);
  toolbar->addAction(actions_->tool_lane_add);
  toolbar->addAction(actions_->tool_lane_form);
  toolbar->addAction(actions_->tool_lane_carve);
  toolbar->addAction(actions_->tool_elevation);
  toolbar->addAction(actions_->tool_create_junction);
  toolbar->addAction(actions_->tool_split);
  toolbar->addAction(actions_->tool_delete);
  toolbar->addAction(actions_->lane_width_editor);
  toolbar->addSeparator();
  toolbar->addAction(actions_->merge_roads);
  toolbar->addAction(actions_->add_from_library);
  toolbar->addSeparator();
  toolbar->addAction(actions_->reset_camera);
  toolbar->addAction(actions_->frame_selection);
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

  options_caption_ = new QLabel(options_bar_);
  options_caption_->setObjectName(QStringLiteral("toolOptionCaption"));
  options_bar_->addWidget(options_caption_);

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

  options_hint_ = new QLabel(options_bar_);
  options_hint_->setObjectName(QStringLiteral("toolOptionHint"));
  options_bar_->addWidget(options_hint_);

  connect(&tool_manager_, &ToolManager::active_changed, this, &MainWindow::update_tool_options);
  update_tool_options();
}

void MainWindow::update_tool_options() {
  const QAction* active = actions_->tool_group->checkedAction();
  const bool create_road = active == actions_->tool_create_road;
  // Create Road gets its option control (labeled template dropdown); every
  // other tool shows its one-line usage as this row's content — the hint
  // text already leads with the tool's name.
  options_caption_->setText(create_road ? tr("Template:") : QString());
  template_action_->setVisible(create_road);
  options_hint_->setText(create_road || active == nullptr ? QString() : active->toolTip());
}

void MainWindow::build_status_bar() {
  statusBar()->addWidget(status_hover_, 1);
  // PERMANENT, not a normal widget: showMessage() hides the normal indications
  // while a transient message is up, and the instruction must survive that —
  // it answers "what can I do with this tool", which stays true while results
  // and refusals come and go. Being permanent also lays it out clear of the
  // message, so the two can never paint over each other.
  status_instruction_ = new QLabel(this);
  status_instruction_->setObjectName(QStringLiteral("status_instruction"));
  statusBar()->addPermanentWidget(status_instruction_);
  // Follow the active tool: its instruction() is the ONE source for "what does
  // this tool do", shown both here and as the viewport corner hint (issue #103
  // — during an interaction the user's eyes are on the viewport). Tools used to
  // emit the same sentence transiently on activate(); they no longer do, so the
  // transient channel carries only results and state-dependent guidance.
  const auto show_instruction = [this] {
    const Tool* tool = tool_manager_.active();
    const QString text = tool == nullptr ? QString() : tool->instruction();
    status_instruction_->setText(text);
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
  case LibraryDropKind::RoadStyle:
  case LibraryDropKind::Assembly:
  case LibraryDropKind::Tree:
  case LibraryDropKind::Signal:
  case LibraryDropKind::Marking:
  case LibraryDropKind::Material:
    if (document_.push_command(std::move(action.command)).has_value()) {
      viewport_->show_toast(action.toast, ToastSeverity::Success);
    } else {
      viewport_->show_toast(tr("Couldn't place that here"), ToastSeverity::Warning);
    }
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
    tour_overlay_->set_target_resolver([this](const QString& key) -> QRect {
      if (main_toolbar_ == nullptr) {
        return {};
      }
      const QList<QAction*> toolbar_actions = main_toolbar_->actions();
      for (QAction* action : toolbar_actions) {
        if (action->iconText() != key) {
          continue;
        }
        QWidget* widget = main_toolbar_->widgetForAction(action);
        if (widget != nullptr && widget->isVisible()) {
          const QPoint top_left = tour_overlay_->mapFromGlobal(widget->mapToGlobal(QPoint(0, 0)));
          return {top_left, widget->size()};
        }
      }
      return {};
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
                   "Licensed under the MIT License — © Robomous.<br><br>"
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

void MainWindow::on_hover(const HoverInfo& info) {
  if (!info.valid) {
    status_hover_->clear();
    return;
  }
  QString text = tr("x %1 m · y %2 m").arg(info.world_x, 0, 'f', 2).arg(info.world_y, 0, 'f', 2);
  if (info.on_road) {
    text += tr("  ·  %1  ·  s %2 m · t %3 m")
                .arg(info.entity)
                .arg(info.s, 0, 'f', 2)
                .arg(info.t, 0, 'f', 2);
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
