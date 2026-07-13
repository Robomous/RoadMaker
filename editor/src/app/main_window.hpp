#pragma once

// QMainWindow shell: menus, toolbar, three docks around a fixed central
// viewport, status bar, drag-and-drop, layout persistence. Arrangement only —
// all state lives in Document/SelectionModel, all logic in the models.

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QStackedWidget>
#include <QToolBar>
#include <QToolButton>
#include <filesystem>

#include "app/actions.hpp"
#include "app/settings.hpp"
#include "app/welcome_widget.hpp"
#include "document/autosave.hpp"
#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/library_list_model.hpp"
#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"
#include "tools/tool_manager.hpp"
#include "viewport/viewport_widget.hpp"

namespace roadmaker::editor {

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  /// `restore_saved_layout` false skips the persisted window geometry/dock
  /// state (screenshot modes need the pristine default arrangement).
  explicit MainWindow(QWidget* parent = nullptr, bool restore_saved_layout = true);

  /// Loads a .xodr; failures land in the Diagnostics panel and a message box.
  void load_file(const std::filesystem::path& path);

  /// The central viewport (screenshot mode drives its camera and capture).
  [[nodiscard]] ViewportWidget* viewport() { return viewport_; }

  /// Screenshot mode: raises the dock with the given objectName (e.g.
  /// "dock.library") to the front of its tab group so it shows in a
  /// whole-window capture. Unknown names no-op.
  void raise_dock_for_capture(const QString& object_name);

  /// Screenshot mode: highlight roads by OpenDRIVE id so the viewport
  /// feedback states render in a capture — `select_odr` gets the strong
  /// selection tint, `hover_odr` the subtle hover brighten. Empty or unknown
  /// ids are ignored.
  void set_capture_highlights(const QString& select_odr, const QString& hover_odr);

  /// Screenshot mode: drops the library item `key` at world (x, y) through the
  /// real drop path, so a capture shows the created geometry + result toast.
  void drop_library_item_for_capture(const QString& key, double world_x, double world_y);

  /// Screenshot mode: activates a tool by id ("select", "edit-nodes",
  /// "create-road", "lane-profile", "elevation", "create-junction", "split",
  /// "delete") so its handle overlay renders in a capture. Unknown ids no-op.
  void activate_tool_for_capture(const QString& tool_id);

  /// Starts the first-run guided tour overlay (Help ▸ Guided Tour, the first
  /// launch, or a `--show-tour` capture). Highlights real toolbar buttons via
  /// their action iconText.
  void start_tour();

protected:
  void changeEvent(QEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  void build_menus();
  void build_toolbar();
  void build_tool_options_bar();
  void build_docks();
  void build_status_bar();
  /// Refreshes the contextual tool-options row for the active tool.
  void update_tool_options();
  /// Captures a small viewport render next to the recent-files entry so the
  /// welcome screen can show it (best effort — a null frame is skipped).
  void save_welcome_thumbnail();
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
#ifdef RM_HAVE_USD
  void export_usd_dialog();
#endif
  void show_about_dialog();
  /// Startup scan for another session's crashed recovery set; prompts
  /// Recover (load + re-point at the original path, dirty) or Discard.
  void check_recovery();
  /// Startup scan for crash reports left by earlier sessions (#84): appends
  /// the crashed session's log tail to the newest report, tells the user
  /// where it lives (local only — nothing is uploaded), offers to open the
  /// folder, and acknowledges every pending report so each is shown once.
  void check_crash_reports();
  void update_recent_files_menu();
  void update_window_title();
  void update_status_entities();
  void on_hover(const HoverInfo& info);

  /// Creates geometry from a library item dropped on the viewport: a road
  /// template arms Create Road at the drop point; a T/X assembly pushes a
  /// standalone-intersection command and toasts the result.
  void on_library_drop(const QString& key, double world_x, double world_y);

  Document document_;
  AutosaveManager autosave_; // after document_: connects to its signals
  SelectionModel selection_;
  SceneTreeModel scene_tree_model_;
  LibraryListModel library_model_;
  DiagnosticsModel diagnostics_model_;
  ToolManager tool_manager_; // declared before viewport_, which references it

  Actions* actions_;
  Settings settings_;
  /// Session-wide "don't ask again" for the move-breaks-links confirm dialog.
  bool suppress_link_break_confirm_ = false;
  /// Owned by tool_manager_; kept for template-dropdown profile changes.
  class CreateRoadTool* create_road_tool_ = nullptr;
  /// Owned by tool_manager_; the Properties panel reads its active node.
  class ElevationTool* elevation_tool_ = nullptr;
  /// Owned by properties_dock_; kept to attach the Elevation tool.
  class PropertiesPanel* properties_panel_ = nullptr;

  /// Central stack: welcome screen until a document exists, viewport after.
  QStackedWidget* central_stack_;
  WelcomeWidget* welcome_;
  ViewportWidget* viewport_;
  QToolBar* options_bar_ = nullptr;
  QLabel* options_caption_ = nullptr;
  QLabel* options_hint_ = nullptr;
  QToolButton* template_button_ = nullptr;
  /// Handle returned by addWidget(template_button_) — visibility toggles go
  /// through the action, not the widget (QToolBar owns the layout).
  QAction* template_action_ = nullptr;
  QDockWidget* scene_dock_;
  QDockWidget* library_dock_;
  QDockWidget* properties_dock_;
  QDockWidget* diagnostics_dock_;
  QDockWidget* profile_dock_;
  QMenu* recent_menu_ = nullptr;
  QLabel* status_hover_;
  QLabel* status_entities_;

  /// Main toolbar — kept so the guided tour can locate an action's button to
  /// highlight (widgetForAction).
  QToolBar* main_toolbar_ = nullptr;
  /// First-run guided-tour overlay; created lazily on first show / Help menu.
  class TourOverlay* tour_overlay_ = nullptr;
  bool tour_checked_ = false; // first-run tour prompt fires at most once
  /// Only interactive launches auto-run the tour — screenshot/capture windows
  /// (restore_saved_layout=false) must never pop it over a render.
  bool allow_first_run_tour_ = false;

  QByteArray default_layout_state_; // captured post-construction for Reset Layout
};

} // namespace roadmaker::editor
