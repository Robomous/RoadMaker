#pragma once

// The 3D viewport: a QOpenGLWidget (3.3 core, requested app-wide via
// QSurfaceFormat in main()) hosting the toolkit-agnostic GLRenderer. Owns the
// camera and the picking state; selection flows OUT through SelectionModel
// and highlight state flows back IN through it — never widget-to-widget.

#include <QImage>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "app/context_menu.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "render/renderer.hpp"
#include "render/scene_builder.hpp"
#include "tools/tool_manager.hpp"
#include "viewport/camera.hpp"
#include "viewport/picking.hpp"

class QPainter;

namespace roadmaker::editor {

/// Status-bar payload for the current mouse position. `world_*` come from the
/// pick ray's ground-plane (z=0) intersection; s/t are set when a road is
/// under the cursor.
struct HoverInfo {
  bool valid = false;
  double world_x = 0.0;
  double world_y = 0.0;
  bool on_road = false;
  QString entity; // "road 5 / lane -1"
  double s = 0.0;
  double t = 0.0;
};

class ViewportWidget : public QOpenGLWidget {
  Q_OBJECT

public:
  ViewportWidget(Document& document,
                 SelectionModel& selection,
                 ToolManager& tools,
                 QWidget* parent = nullptr);
  ~ViewportWidget() override;

  ViewportWidget(const ViewportWidget&) = delete;
  ViewportWidget& operator=(const ViewportWidget&) = delete;
  ViewportWidget(ViewportWidget&&) = delete;
  ViewportWidget& operator=(ViewportWidget&&) = delete;

public slots:
  void reset_camera();

  /// Frames the selected road/lane, or the whole scene without a selection.
  void frame_selection();

  /// Active-tool hint drawn in the viewport corner (mirrors the status bar —
  /// the user's eyes are on the viewport during a tool interaction, issue
  /// #103 discoverability). Empty text clears it.
  void set_hint(const QString& text);

public:
  [[nodiscard]] QString hint() const { return hint_text_; }

  /// Camera preset for scripted captures: "top" (plan view, north up) or
  /// "orbit" (the default 3/4 view). Unknown names keep the current view.
  void set_camera_preset(const QString& preset);

  /// Screenshot mode only: force a road-level hover highlight so the hover
  /// feedback state can be captured without a live cursor. Locks the hover so
  /// spurious enter/leave/move events around capture can't clear it;
  /// interactive use never calls this, so live hover is unaffected.
  void set_hover_preview(RoadId road) {
    hovered_road_ = road;
    hovered_lane_ = {};
    hover_locked_ = true;
    update();
  }

  /// Renders the current scene into an offscreen framebuffer and returns the
  /// frame (screenshot mode, docs/contributing/pull-requests.md visual
  /// evidence). Runs any pending scene upload first via paintGL. Null image
  /// when GL never initialized.
  [[nodiscard]] QImage capture_frame();

signals:
  void hover_changed(const roadmaker::editor::HoverInfo& info);

  /// A right-click without an orbit drag: the descriptor context under the
  /// cursor and the global position to popup at. MainWindow assembles and
  /// shows the QMenu (it holds the Actions the item closures need).
  void context_menu_requested(const roadmaker::editor::MenuContext& context,
                              const QPoint& global_pos);

protected:
  void initializeGL() override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void leaveEvent(QEvent* event) override;

private:
  struct UploadedItem {
    RenderMeshHandle handle;
    RoadId road;
    LaneId lane;
  };

  void rebuild_scene();

  /// Re-uploads only the pending roads' items (GL context must be current —
  /// called from paintGL, like rebuild_scene). Camera stays put.
  void apply_pending_road_updates();

  /// Refreshes the cached AABB slots of `roads` (index-parallel to
  /// document_.mesh().roads; replaced-in-place roads keep their index).
  void refresh_road_aabbs(const std::vector<RoadId>& roads);

  [[nodiscard]] HighlightState item_state(const UploadedItem& item) const;
  [[nodiscard]] Ray ray_through(const QPointF& pos) const;

  /// Ground-plane (z=0) world point under a viewport pixel, or nullopt when the
  /// ray misses the plane (or lies beyond `max_t` — used to reject near-horizon
  /// rays for the anchored pan). Shared by hover, tool events, and MMB pan.
  [[nodiscard]] std::optional<std::array<double, 3>>
  ground_point_at(const QPointF& pos, double max_t = std::numeric_limits<double>::infinity()) const;
  void update_hover(const QPointF& pos);

  /// Sets the road-level hover highlight, repainting only when it changes.
  /// A road-level hover leaves the hovered lane invalid (the whole road
  /// brightens); pass an invalid id to clear.
  void set_hovered_road(RoadId road);

  /// Builds the right-click MenuContext under a viewport pixel: node handle of
  /// a selected road (priority), else the road/lane pick + its station.
  [[nodiscard]] MenuContext build_menu_context(const QPointF& pos) const;

  /// Translates a Qt mouse event into the tool seam's abstract event:
  /// ground-plane (z=0) world position, lane-patch pick, buttons, modifiers.
  [[nodiscard]] ToolEvent make_tool_event(const QMouseEvent* event) const;

  /// Re-subscribes preview_changed to the newly active tool.
  void attach_active_tool();

  /// Replaces the overlay meshes with the active tool's PreviewGeometry
  /// (GL context must be current — called from paintGL). Uploads the line
  /// overlays to GL and stashes the handle knobs for draw_handles().
  void upload_tool_preview();

  /// Paints the active tool's handle knobs as screen-space themed sprites
  /// (idle/hovered/grabbed), projected from the kernel frame. QPainter over
  /// the GL frame, like the hint card.
  void draw_handles(QPainter& painter) const;

  Document& document_;
  SelectionModel& selection_;
  ToolManager& tools_;

  std::unique_ptr<Renderer> renderer_;
  OrbitCamera camera_;
  std::vector<UploadedItem> items_;
  std::vector<RoadAabb> road_aabbs_;
  SceneBounds scene_bounds_;

  bool gl_ready_ = false;
  bool scene_dirty_ = false;

  /// Roads awaiting a partial re-upload on the next paint (deduplicated
  /// there); ignored while a full rebuild is pending.
  std::vector<RoadId> pending_roads_;

  /// rebuild_scene() re-frames the camera only after a document load —
  /// edit-driven rebuilds must not yank the view.
  bool frame_on_rebuild_ = true;

  /// Overlay meshes for the active tool's preview line geometry, replaced
  /// every paint. The handle knobs are drawn separately (draw_handles).
  std::vector<RenderMeshHandle> preview_handles_;

  /// The active tool's handle knobs (kernel frame), stashed by
  /// upload_tool_preview and painted as screen sprites by draw_handles.
  std::vector<Handle> handle_overlays_;
  QMetaObject::Connection preview_connection_;
  QMetaObject::Connection cursor_connection_;

  QPoint last_mouse_pos_;

  /// Ground point grabbed on middle-mouse press; the anchored pan keeps it
  /// pinned under the cursor. nullopt when the press ray missed the ground
  /// (near-horizon) — the pan then falls back to a scaled view-plane shift.
  std::optional<std::array<double, 3>> pan_anchor_world_;

  /// Right-mouse click-vs-drag disambiguation: press position, and whether the
  /// drag crossed the threshold (orbiting) — a release without it pops the menu.
  QPoint rmb_press_pos_;
  bool rmb_orbiting_ = false;

  /// Corner hint text (set_hint); painted over the GL frame in paintGL.
  QString hint_text_;

  /// Entity under the cursor, tracked by update_hover for the hover highlight
  /// (invalid = nothing hovered). A lane-level hover sets both; a road-level
  /// hover leaves the lane invalid. Selection (SelectionModel) takes priority
  /// over these when both apply to the same mesh.
  RoadId hovered_road_;
  LaneId hovered_lane_;

  /// Set by set_hover_preview (screenshot mode): update_hover then leaves the
  /// forced hover in place so a capture isn't wiped by a spurious event.
  bool hover_locked_ = false;
};

} // namespace roadmaker::editor
