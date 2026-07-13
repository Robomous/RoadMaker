#include "app/main_window.hpp"

#include "roadmaker/version.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
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
#include "document/library_manifest.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/library_panel.hpp"
#include "panels/profile_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"
#include "tools/create_junction_tool.hpp"
#include "tools/create_road_tool.hpp"
#include "tools/delete_tool.hpp"
#include "tools/edit_nodes_tool.hpp"
#include "tools/elevation_tool.hpp"
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
  connect(actions_->about, &QAction::triggered, this, &MainWindow::show_about_dialog);
  connect(viewport_, &ViewportWidget::hover_changed, this, &MainWindow::on_hover);
  connect(viewport_,
          &ViewportWidget::context_menu_requested,
          this,
          [this](const MenuContext& context, const QPoint& global_pos) {
            ContextMenuDeps deps{document_, selection_, *actions_};
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
  auto select_tool = std::make_unique<SelectTool>(document_, selection_);
  wire_status(select_tool.get());
  // Moving a road that links to roads staying put breaks those links. Confirm
  // once (with a session-wide "don't ask again"), BEFORE the preview begins —
  // a modal opened mid-drag swallows the mouse-release.
  select_tool->set_link_break_confirm([this]() -> bool {
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
  });
  SelectTool* select_tool_ptr = select_tool.get();
  tool_manager_.register_tool(ToolId::Select, std::move(select_tool));
  connect(actions_->tool_select, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Select);
  });
  auto create_road_tool = std::make_unique<CreateRoadTool>(document_);
  create_road_tool_ = create_road_tool.get();
  wire_status(create_road_tool.get());
  tool_manager_.register_tool(ToolId::CreateRoad, std::move(create_road_tool));
  connect(actions_->tool_create_road, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::CreateRoad);
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
  auto lane_profile_tool = std::make_unique<LaneProfileTool>(selection_);
  wire_status(lane_profile_tool.get());
  tool_manager_.register_tool(ToolId::LaneProfile, std::move(lane_profile_tool));
  connect(actions_->tool_lane_profile, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::LaneProfile);
  });
  auto elevation_tool = std::make_unique<ElevationTool>(document_, selection_);
  elevation_tool_ = elevation_tool.get();
  wire_status(elevation_tool.get());
  tool_manager_.register_tool(ToolId::Elevation, std::move(elevation_tool));
  connect(actions_->tool_elevation, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Elevation);
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
      statusBar()->showMessage(
          tr("Cannot merge: %1").arg(QString::fromStdString(merged.error().message)), 5000);
      return;
    }
    selection_.select({.road = a, .lane = LaneId{}}, SelectMode::Replace);
    statusBar()->showMessage(tr("Merged into road %1 — Ctrl+Z to undo").arg(surviving), 5000);
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
  library_dock_->setWidget(new LibraryPanel(library_model_, library_dock_));
  addDockWidget(Qt::LeftDockWidgetArea, library_dock_);
  tabifyDockWidget(scene_dock_, library_dock_);
  scene_dock_->raise(); // Scene tree is the default front tab

  properties_dock_ = new QDockWidget(tr("Properties"), this);
  properties_dock_->setObjectName(QStringLiteral("dock.properties"));
  properties_panel_ = new PropertiesPanel(document_, selection_, properties_dock_);
  properties_dock_->setWidget(properties_panel_);
  properties_dock_->widget()->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, properties_dock_);

  profile_dock_ = new QDockWidget(tr("Profile"), this);
  profile_dock_->setObjectName(QStringLiteral("dock.profile"));
  profile_dock_->setWidget(new ProfilePanel(document_, selection_, profile_dock_));
  addDockWidget(Qt::BottomDockWidgetArea, profile_dock_);
  profile_dock_->hide(); // opt-in via the View menu — vertical design is occasional

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

  QMenu* view_menu = menuBar()->addMenu(tr("&View"));
  view_menu->addAction(scene_dock_->toggleViewAction());
  view_menu->addAction(library_dock_->toggleViewAction());
  view_menu->addAction(properties_dock_->toggleViewAction());
  view_menu->addAction(diagnostics_dock_->toggleViewAction());
  view_menu->addAction(profile_dock_->toggleViewAction());
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_camera);
  view_menu->addAction(actions_->frame_selection);
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_layout);

  QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
  help_menu->addAction(actions_->about);
}

void MainWindow::build_toolbar() {
  // Labeled toolbar (ui-design.md): 28 px icons with the action's iconText
  // under each, grouped File | Tools | View — a new user can read what
  // every button does.
  QToolBar* toolbar = addToolBar(tr("Main"));
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
  toolbar->addAction(actions_->tool_create_road);
  toolbar->addAction(actions_->tool_edit_nodes);
  toolbar->addAction(actions_->tool_lane_profile);
  toolbar->addAction(actions_->tool_elevation);
  toolbar->addAction(actions_->tool_create_junction);
  toolbar->addAction(actions_->tool_split);
  toolbar->addAction(actions_->tool_delete);
  toolbar->addSeparator();
  toolbar->addAction(actions_->merge_roads);
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
      {QStringLiteral("create-road"), ToolId::CreateRoad},
      {QStringLiteral("edit-nodes"), ToolId::EditNodes},
      {QStringLiteral("lane-profile"), ToolId::LaneProfile},
      {QStringLiteral("elevation"), ToolId::Elevation},
      {QStringLiteral("create-junction"), ToolId::CreateJunction},
      {QStringLiteral("split"), ToolId::Split},
      {QStringLiteral("delete"), ToolId::Delete},
  };
  if (const auto found = kTools.find(tool_id); found != kTools.end()) {
    tool_manager_.set_active(found->second);
  }
}

void MainWindow::raise_dock_for_capture(const QString& object_name) {
  for (QDockWidget* dock : findChildren<QDockWidget*>()) {
    if (dock->objectName() == object_name) {
      dock->raise();
      return;
    }
  }
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
  const QString suggested =
      document_.has_file() ? document_.file_path() : QStringLiteral("untitled.xodr");
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
  save_welcome_thumbnail();
  statusBar()->showMessage(tr("Saved %1").arg(document_.file_path()), 5000);
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
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open OpenDRIVE file"), QString(), tr("OpenDRIVE (*.xodr);;All files (*)"));
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
    statusBar()->showMessage(tr("Exported %1").arg(path), 5000);
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
    statusBar()->showMessage(tr("Exported %1").arg(path), 5000);
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
  setWindowTitle(tr("%1[*] — RoadMaker").arg(name));
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
