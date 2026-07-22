#pragma once

// QMainWindow shell: menus, toolbar, three docks around a fixed central
// viewport, status bar, drag-and-drop, layout persistence. Arrangement only —
// all state lives in Document/SelectionModel, all logic in the models.

#include "roadmaker/edit/markings.hpp"

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QToolBar>
#include <QToolButton>
#include <filesystem>
#include <optional>

#include "app/actions.hpp"
#include "app/elided_label.hpp"
#include "app/settings.hpp"
#include "app/welcome_widget.hpp"
#include "document/autosave.hpp"
#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/library_list_model.hpp"
#include "document/project.hpp"
#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"
#include "tools/tool_manager.hpp"
#include "viewport/viewport_widget.hpp"

namespace roadmaker::editor {

namespace help {
class HelpViewer;
}

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

  /// Screenshot mode: previews (does not commit) the library item `key` mid-drag
  /// at world (x, y) through the real drag path, so a capture shows the
  /// world-anchored drop ghost sitting at the resolved landing point (A1).
  void preview_library_drag_for_capture(const QString& key, double world_x, double world_y);

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
  /// File ▸ New Project…: directory picker + name prompt, then Project::create.
  void new_project_dialog();
  /// File ▸ Open Project…: directory picker, then open_project_dir.
  void open_project_dialog();
  /// Opens the project at `dir` (error box on failure) and adopts it.
  void open_project_dir(const QString& dir);
  /// Makes `project` the active project: records it in recent projects,
  /// applies its Library overlay, and updates the title + welcome screen.
  void adopt_project(Project project);
  /// Drops the active project association: removes the Library overlay and
  /// reverts the title + welcome screen. No-op when no project is active.
  void clear_project();
  /// (Re)loads the active project's assets/library/manifest.json into the
  /// Library model as an overlay; clears the overlay when the project has
  /// none (or none parses).
  void apply_project_overlay();
  /// Loads the active project's overlay manifest, or a fresh one when none
  /// exists yet — the base for a crosswalk-asset create/edit write (p3-s2).
  [[nodiscard]] LibraryManifest load_or_create_overlay_manifest() const;
  /// The CrosswalkParams the junction "Add crosswalks" op uses — the default
  /// crosswalk asset from the merged Library (p3-s2).
  [[nodiscard]] edit::CrosswalkParams resolve_default_crosswalk_params() const;
  /// The default Stencil asset from the merged Library — the glyph the Marking
  /// Point tool places (p3-s4). An empty item when the Library has none.
  [[nodiscard]] LibraryItem resolve_default_stencil_item() const;
  /// The default Tree asset from the merged Library — the prop the Prop Point and
  /// Prop Curve tools place (p6-s4). An empty item when the Library has none.
  [[nodiscard]] LibraryItem resolve_default_prop_item() const;

  /// The Library's default signal asset (p4-s7, issue #228) — what a Signal
  /// tool click on a road places, so the tool and the Library drop land the
  /// same head.
  [[nodiscard]] LibraryItem resolve_default_signal_item() const;
  [[nodiscard]] LibraryItem resolve_default_sign_item() const;
  /// The default crosswalk/marking asset from the merged Library — the asset the
  /// Marking Curve tool authors (p3-s4). An empty item when the Library has none.
  [[nodiscard]] LibraryItem resolve_default_marking_curve_item() const;
  /// Creates a new project-overlay crosswalk asset (from library defaults),
  /// saves it, refreshes the Library, and opens its editor. Toasts when no
  /// project is open.
  void create_crosswalk_asset();
  /// Persists an edited crosswalk asset into the project overlay and propagates
  /// the change to every following instance in one undoable command.
  void commit_crosswalk_asset(const LibraryItem& item);
  /// Creates a new project-overlay prop-set asset (default mixed entries),
  /// saves it, refreshes the Library, and opens its editor. Toasts when no
  /// project is open.
  void create_prop_set_asset();
  /// Persists an edited prop-set asset into the project overlay. No propagation:
  /// baked props never reference the set, so an edit only affects future
  /// scatters (contrast commit_crosswalk_asset, which re-materializes instances).
  void commit_prop_set_asset(const LibraryItem& item);
  /// Adopts the project containing `scene_path` (or clears the association
  /// for a standalone scene). Runs after every load and save.
  void associate_project_for(const std::filesystem::path& scene_path);
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
  /// Opens (creating on first use) the in-app user guide, on the given slug.
  void show_help(const QString& slug = QStringLiteral("index"));
  /// The objectName of the QDockWidget that owns the keyboard focus, or an
  /// empty string when focus is not inside a dock. F1 feeds this to
  /// help::context_page so the focused panel's page wins over the active tool.
  [[nodiscard]] QString help_context_dock() const;
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

  /// Rebuilds the viewport's signal-phase overlay from the phase panel's current
  /// playhead (head colors, moving roads, gate links), or clears it when the
  /// pane is hidden or the panel has no signalized junction. Pane-driven, so it
  /// works under any active tool.
  void update_signal_phase_overlay();

  /// Creates geometry from a library item dropped on the viewport: a road
  /// template arms Create Road at the drop point; a T/X assembly pushes a
  /// standalone-intersection command and toasts the result.
  void on_library_drop(const QString& key, double world_x, double world_y);

  /// Positions the drop ghost while a library item is dragged over the viewport:
  /// resolves the key through the same resolve_library_drop the drop uses and
  /// pushes the resolved landing point back to the viewport (ghost==commit).
  void on_library_drag_moved(const QString& key, double world_x, double world_y);

  Document document_;
  AutosaveManager autosave_; // after document_: connects to its signals
  /// The active project (p6-s1): a directory association, not a document —
  /// opening a scene inside a project directory auto-adopts it, opening a
  /// standalone scene drops it. Drives the Library overlay, the window
  /// title, and where the file dialogs default.
  std::optional<Project> project_;
  SelectionModel selection_;
  SceneTreeModel scene_tree_model_;
  LibraryListModel library_model_;
  /// Resolves a crosswalk asset's material to a paint tint when propagating an
  /// asset edit to its instances (p3-s2).
  MaterialCatalog crosswalk_materials_;
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
  class LibraryPanel* library_panel_ = nullptr;

  /// Central stack: welcome screen until a document exists, viewport after.
  QStackedWidget* central_stack_;
  WelcomeWidget* welcome_;
  ViewportWidget* viewport_;
  QToolBar* options_bar_ = nullptr;
  QLabel* options_caption_ = nullptr;
  QToolButton* template_button_ = nullptr;
  /// Handles returned by addWidget() — visibility toggles go through the
  /// action, not the widget (QToolBar owns the layout). Both flip together so
  /// the row's content is either "Template: <dropdown>" or nothing; nothing in
  /// any toolbar may have tool-dependent width (issue #332).
  QAction* options_caption_action_ = nullptr;
  QAction* template_action_ = nullptr;
  QDockWidget* scene_dock_;
  QDockWidget* library_dock_;
  QDockWidget* properties_dock_;
  QDockWidget* diagnostics_dock_;
  QDockWidget* editor2d_dock_;
  class Editor2DHost* editor2d_host_ = nullptr;
  class SignalPhaseEditorPage* phase_page_ = nullptr;
  QMenu* recent_menu_ = nullptr;
  QLabel* status_hover_;
  /// Persistent per-tool instruction line (what click/drag/modifiers do right
  /// now). Distinct from the transient status_message channel, which keeps its
  /// 5 s showMessage lane for state-dependent guidance and results. Elides
  /// rather than widen the bar — its length varies with the tool.
  ElidedLabel* status_instruction_ = nullptr;
  QLabel* status_entities_;

  /// The two generated toolbar rows (p1-s5): row 1 authors the network, row 2
  /// holds the scene layers. Kept so the guided tour can locate an action's
  /// button to highlight (widgetForAction) — it must search BOTH.
  QToolBar* main_toolbar_ = nullptr;
  QToolBar* layers_toolbar_ = nullptr;
  /// First-run guided-tour overlay; created lazily on first show / Help menu.
  class TourOverlay* tour_overlay_ = nullptr;
  bool tour_checked_ = false; // first-run tour prompt fires at most once
  /// In-app user-guide window; created lazily on first Help ▸ User Guide / F1.
  QPointer<help::HelpViewer> help_viewer_;
  /// Only interactive launches auto-run the tour — screenshot/capture windows
  /// (restore_saved_layout=false) must never pop it over a render.
  bool allow_first_run_tour_ = false;

  QByteArray default_layout_state_; // captured post-construction for Reset Layout
};

} // namespace roadmaker::editor
