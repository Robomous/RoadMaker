#include "app/main_window.hpp"

#include "roadmaker/version.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>

#include "app/icons.hpp"
#include "panels/diagnostics_panel.hpp"
#include "panels/properties_panel.hpp"
#include "panels/scene_tree_panel.hpp"
#include "tools/edit_nodes_tool.hpp"
#include "tools/select_tool.hpp"

namespace roadmaker::editor {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), selection_(document_), scene_tree_model_(document_),
      diagnostics_model_(document_), actions_(new Actions(*document_.undo_stack(), this)),
      viewport_(new ViewportWidget(document_, selection_, tool_manager_, this)),
      status_hover_(new QLabel(this)), status_entities_(new QLabel(this)) {
  setAcceptDrops(true);
  setCentralWidget(viewport_); // central widget: always visible, never dockable
  resize(1600, 1000);

  build_docks();
  build_menus();
  build_toolbar();
  build_status_bar();

  connect(actions_->open, &QAction::triggered, this, &MainWindow::open_file_dialog);
  connect(actions_->export_glb, &QAction::triggered, this, &MainWindow::export_file_dialog);
  connect(actions_->quit, &QAction::triggered, this, &QMainWindow::close);
  connect(actions_->reset_camera, &QAction::triggered, viewport_, &ViewportWidget::reset_camera);
  connect(
      actions_->frame_selection, &QAction::triggered, viewport_, &ViewportWidget::frame_selection);
  connect(actions_->about, &QAction::triggered, this, &MainWindow::show_about_dialog);
  connect(viewport_, &ViewportWidget::hover_changed, this, &MainWindow::on_hover);
  connect(&document_, &Document::loaded, this, [this] {
    actions_->export_glb->setEnabled(true);
    update_window_title();
    update_status_entities();
    diagnostics_dock_->setVisible(!document_.diagnostics().empty() ||
                                  diagnostics_dock_->isVisible());
  });

  // Editing tools (M2). Select/Move is the default; guidance lands in the
  // status bar via the tool's status_message.
  auto select_tool = std::make_unique<SelectTool>(document_, selection_);
  connect(select_tool.get(), &Tool::status_message, this, [this](const QString& text) {
    statusBar()->showMessage(text, 5000);
  });
  tool_manager_.register_tool(ToolId::Select, std::move(select_tool));
  connect(actions_->tool_select, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::Select);
  });
  auto edit_nodes_tool = std::make_unique<EditNodesTool>(document_, selection_);
  connect(edit_nodes_tool.get(), &Tool::status_message, this, [this](const QString& text) {
    statusBar()->showMessage(text, 5000);
  });
  tool_manager_.register_tool(ToolId::EditNodes, std::move(edit_nodes_tool));
  connect(actions_->tool_edit_nodes, &QAction::triggered, this, [this] {
    tool_manager_.set_active(ToolId::EditNodes);
  });
  tool_manager_.set_active(ToolId::Select);

  // The freshly-built arrangement is the canonical layout Reset Layout
  // restores; user geometry (if any) is applied on top of it.
  default_layout_state_ = saveState();
  connect(actions_->reset_layout, &QAction::triggered, this, [this] {
    restoreState(default_layout_state_);
  });
  if (!settings_.restore_window(*this)) {
    // First run (no saved layout): keep the default arrangement built above.
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

  properties_dock_ = new QDockWidget(tr("Properties"), this);
  properties_dock_->setObjectName(QStringLiteral("dock.properties"));
  properties_dock_->setWidget(new PropertiesPanel(document_, selection_, properties_dock_));
  properties_dock_->widget()->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, properties_dock_);

  diagnostics_dock_ = new QDockWidget(tr("Diagnostics"), this);
  diagnostics_dock_->setObjectName(QStringLiteral("dock.diagnostics"));
  diagnostics_dock_->setWidget(
      new DiagnosticsPanel(document_, diagnostics_model_, selection_, diagnostics_dock_));
  addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock_);
  diagnostics_dock_->hide(); // collapsed by default until diagnostics arrive
}

void MainWindow::build_menus() {
  QMenu* file_menu = menuBar()->addMenu(tr("&File"));
  file_menu->addAction(actions_->open);
  recent_menu_ = file_menu->addMenu(tr("Open &Recent"));
  update_recent_files_menu();
  file_menu->addSeparator();
  file_menu->addAction(actions_->export_glb);
  file_menu->addSeparator();
  file_menu->addAction(actions_->quit);

  QMenu* edit_menu = menuBar()->addMenu(tr("&Edit"));
  edit_menu->addAction(actions_->undo);
  edit_menu->addAction(actions_->redo);

  QMenu* view_menu = menuBar()->addMenu(tr("&View"));
  view_menu->addAction(scene_dock_->toggleViewAction());
  view_menu->addAction(properties_dock_->toggleViewAction());
  view_menu->addAction(diagnostics_dock_->toggleViewAction());
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_camera);
  view_menu->addAction(actions_->frame_selection);
  view_menu->addSeparator();
  view_menu->addAction(actions_->reset_layout);

  QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
  help_menu->addAction(actions_->about);
}

void MainWindow::build_toolbar() {
  QToolBar* toolbar = addToolBar(tr("Main"));
  toolbar->setObjectName(QStringLiteral("toolbar.main"));
  toolbar->setIconSize(QSize(16, 16));
  toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  toolbar->setMovable(false);
  toolbar->addAction(actions_->open);
  toolbar->addAction(actions_->export_glb);
  toolbar->addSeparator();
  toolbar->addAction(actions_->tool_select);
  toolbar->addAction(actions_->tool_edit_nodes);
  toolbar->addSeparator();
  toolbar->addAction(actions_->reset_camera);
  toolbar->addAction(actions_->frame_selection);
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

void MainWindow::show_about_dialog() {
  QMessageBox::about(
      this,
      tr("About RoadMaker"),
      tr("<b>RoadMaker</b> — open source road-network authoring toolkit.<br>"
         "Kernel %1<br><br>"
         "Licensed under the MIT License — © Robomous.<br><br>"
         "Built with Qt %2, used under the GNU LGPLv3 as dynamically linked "
         "libraries. You may replace the bundled Qt libraries with your own "
         "builds; see THIRD_PARTY_LICENSES.md in the distribution.")
          .arg(QString::fromUtf8(roadmaker::version().data(),
                                 static_cast<qsizetype>(roadmaker::version().size())),
               QStringLiteral(QT_VERSION_STR)));
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
  setWindowTitle(document_.has_file()
                     ? tr("%1 — RoadMaker").arg(QFileInfo(document_.file_path()).fileName())
                     : tr("RoadMaker"));
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
