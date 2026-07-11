#pragma once

// QMainWindow shell: menus, toolbar, three docks around a fixed central
// viewport, status bar, drag-and-drop, layout persistence. Arrangement only —
// all state lives in Document/SelectionModel, all logic in the models.

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <filesystem>

#include "app/actions.hpp"
#include "app/settings.hpp"
#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"
#include "tools/tool_manager.hpp"
#include "viewport/viewport_widget.hpp"

namespace roadmaker::editor {

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);

  /// Loads a .xodr; failures land in the Diagnostics panel and a message box.
  void load_file(const std::filesystem::path& path);

protected:
  void changeEvent(QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  void build_menus();
  void build_toolbar();
  void build_docks();
  void build_status_bar();
  void new_file();
  void open_file_dialog();
  /// Save / Save As… — return false when the user cancels or the write
  /// fails, so confirm_discard() can abort the enclosing New/close.
  bool save_file();
  bool save_file_as();
  bool save_to(const std::filesystem::path& path);
  /// True when it is safe to drop the current document: clean stack, or the
  /// user chose Save (and it succeeded) or Discard.
  bool confirm_discard();
  void export_file_dialog();
  void show_about_dialog();
  void update_recent_files_menu();
  void update_window_title();
  void update_status_entities();
  void on_hover(const HoverInfo& info);

  Document document_;
  SelectionModel selection_;
  SceneTreeModel scene_tree_model_;
  DiagnosticsModel diagnostics_model_;
  ToolManager tool_manager_; // declared before viewport_, which references it

  Actions* actions_;
  Settings settings_;
  /// Owned by tool_manager_; kept for template-dropdown profile changes.
  class CreateRoadTool* create_road_tool_ = nullptr;
  /// Owned by tool_manager_; the Properties panel reads its active node.
  class ElevationTool* elevation_tool_ = nullptr;
  /// Owned by properties_dock_; kept to attach the Elevation tool.
  class PropertiesPanel* properties_panel_ = nullptr;

  ViewportWidget* viewport_;
  QDockWidget* scene_dock_;
  QDockWidget* properties_dock_;
  QDockWidget* diagnostics_dock_;
  QMenu* recent_menu_ = nullptr;
  QLabel* status_hover_;
  QLabel* status_entities_;

  QByteArray default_layout_state_; // captured post-construction for Reset Layout
};

} // namespace roadmaker::editor
