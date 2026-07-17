#pragma once

// The 3D viewport: a QOpenGLWidget (3.3 core, requested app-wide via
// QSurfaceFormat in main()) hosting the toolkit-agnostic GLRenderer. Owns the
// camera and the picking state; selection flows OUT through SelectionModel
// and highlight state flows back IN through it — never widget-to-widget.

#include <QElapsedTimer>
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
#include "viewport/gizmo.hpp"
#include "viewport/nav_controller.hpp"
#include "viewport/picking.hpp"
#include "viewport/toast_queue.hpp"

class QPainter;
class QTimer;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;

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

  /// Frames exactly what is selected (per kind — a signal frames the signal,
  /// a lane its own patch, a junction its floor); with no selection, frames the
  /// whole scene keeping the viewing angle; in an empty scene, returns to the
  /// origin pivot. GW-1 steps 7-9.
  void frame_selection();

  /// Moves the pivot to the point under the cursor, preserving the viewing
  /// angle AND the zoom distance (GW-1 step 10). A cursor outside the viewport,
  /// or over nothing at all, leaves the camera alone.
  void frame_cursor();

  /// Switches the projection; a no-op when already in `mode`. The pivot-plane
  /// scale is shared between modes, so the content does not jump (GW-1 step 11).
  void set_projection(roadmaker::editor::ProjectionMode mode);

  /// Snaps the view to a cardinal direction, preserving pivot and distance
  /// (GW-1 steps 12-13).
  void look_from(roadmaker::editor::CardinalView view);

  /// Switches between the daytime "Textured" render mode (new default —
  /// hemisphere + directional lighting, dimmed reference grid) and the flat
  /// "Sober" mode (the M2 look, kept as the packaging/CI smoke path). Geometry
  /// is unchanged, so switching is instant and needs no re-mesh.
  void set_textured_rendering(bool textured);

  [[nodiscard]] bool textured_rendering() const { return textured_rendering_; }

  /// Active-tool hint drawn in the viewport corner (mirrors the status bar —
  /// the user's eyes are on the viewport during a tool interaction, issue
  /// #103 discoverability). Empty text clears it.
  void set_hint(const QString& text);

  /// Shows a transient toast in the viewport (queued, themed, auto-fading).
  /// The single place editor feedback ("Merged", "Saved", a refusal) surfaces
  /// over the scene instead of only in the status bar.
  void show_toast(const QString& text, ToastSeverity severity = ToastSeverity::Info);

  /// Positions the drop ghost at a resolved landing point while a library item
  /// is dragged over the viewport. MainWindow resolves the drag through the same
  /// resolve_library_drop the drop commit uses, so the ghost marks exactly where
  /// the element lands (ghost==commit); `valid` false tints it as a rejected drop.
  void set_drop_preview(double world_x, double world_y, bool valid);

  /// Clears the drop ghost (drag left the viewport or completed).
  void clear_drop_preview();

  /// Highlights `road` as the target of an in-flight road-style drag (the road
  /// the style would apply to). Rendered through the same Hover feedback state
  /// as a live hover; a mouse-move hover does not fire during a Qt drag, so
  /// MainWindow drives this explicitly from the drag-move handler.
  void set_drag_target_road(RoadId road);

  /// Clears the road-style drag highlight (drag left the viewport, dropped, or
  /// moved off any road).
  void clear_drag_target_road();

public:
  [[nodiscard]] QString hint() const { return hint_text_; }

  /// Camera preset for scripted captures: "top" (plan view, north up),
  /// "ortho" (plan view in orthographic projection), or "orbit" (the default
  /// 3/4 view). Unknown names keep the current view.
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

  /// A library item was dropped on the viewport at the ground point
  /// (world_x, world_y). MainWindow resolves the key and creates the geometry
  /// (it owns the library model, document, and tools).
  void library_item_dropped(const QString& key, double world_x, double world_y);

  /// A library item is being dragged over the viewport; its resolved world
  /// point (surface raycast, falling back to the ground plane) is
  /// (world_x, world_y). MainWindow resolves the key and pushes back a drop
  /// preview (set_drop_preview) so the ghost shows where it will land.
  void library_item_drag_moved(const QString& key, double world_x, double world_y);

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
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dragLeaveEvent(QDragLeaveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  struct UploadedItem {
    RenderMeshHandle handle;
    RoadId road;
    LaneId lane;
    ObjectId object;
    SignalId signal;
    JunctionId junction;
    SurfaceId surface_id;
    SurfaceKind surface = SurfaceKind::Untextured;
  };

  /// The textured-mode Material for an item: the resolved surface texture
  /// (asphalt/concrete) or bright unlit paint for markings. Returns a default
  /// (flat) Material in Sober mode or when the textures failed to load.
  [[nodiscard]] Material material_for(SurfaceKind surface) const;

  void rebuild_scene();

  /// Pushes the current render mode to the renderer: the Environment (textured
  /// vs sober lighting) and a grid-dimmed backdrop in textured mode.
  void apply_render_mode();

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

  /// World point under a viewport pixel for a placement drop: the actual surface
  /// under the cursor via pick() (road/junction/prop), falling back to the ground
  /// plane in empty space. This is the projection the hover readout uses — the
  /// drop shares it so an element lands on the surface it visually points at,
  /// not on a hidden z=0 plane. nullopt only when both miss.
  [[nodiscard]] std::optional<std::array<double, 3>> drop_world_point(const QPointF& pos) const;
  void update_hover(const QPointF& pos);

  /// Sets the road-level hover highlight, repainting only when it changes.
  /// A road-level hover leaves the hovered lane invalid (the whole road
  /// brightens); pass an invalid id to clear.
  void set_hovered_road(RoadId road);

  /// Sets the hovered prop/object (repainting only on change); an object hover
  /// clears the road/lane hover and vice versa. Pass an invalid id to clear.
  void set_hovered_object(ObjectId object);

  /// Sets the hovered signal (repainting only on change); mutually exclusive
  /// with the road/lane/object hover. Pass an invalid id to clear.
  void set_hovered_signal(SignalId signal);

  /// Sets the hovered junction floor (repainting only on change). Mutually
  /// exclusive with the road/object hover; pass an invalid id to clear.
  void set_hovered_junction(JunctionId junction);

  /// Sets the hovered ground surface (#215, repainting only on change).
  /// Mutually exclusive with the road/object/junction hover; pass an invalid
  /// id to clear.
  void set_hovered_surface(SurfaceId surface);

  /// Builds the right-click MenuContext under a viewport pixel: node handle of
  /// a selected road (priority), else the road/lane pick + its station.
  [[nodiscard]] MenuContext build_menu_context(const QPointF& pos) const;

  /// Grabs (or drops) the ground point the anchored pan pins under the cursor,
  /// to match the gesture the NavController just resolved. Called after every
  /// press/release, since a chord can enter or leave Pan on either.
  void sync_pan_anchor(const QPointF& pos);

  /// Applies one mouse-move frame of the live navigation gesture to the camera:
  /// orbit, ground-anchored pan, drag-zoom, or pivot lift.
  void apply_nav_move(const QPoint& delta, const QPointF& pos);

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

  /// Themed top-left tool-hint card; fades out after an idle stretch.
  void draw_hint_card(QPainter& painter) const;

  /// Themed top-center transient toasts (queued, severity-colored, fading).
  /// Non-const: pulls the live set from the queue, which prunes expired ones.
  void draw_toasts(QPainter& painter);

  /// A world-anchored marker at the resolved landing point (drop_preview_)
  /// while a library item is dragged over the viewport — projected to screen so
  /// it sits where the element will commit; tinted as a rejection when invalid.
  void draw_drag_ghost(QPainter& painter) const;

  // --- transform gizmo (A3, #177) --------------------------------------------
  /// The single transformable entity under the gizmo: a road or a prop, with its
  /// world pivot. Present only when the Move tool is active with exactly one such
  /// entity selected; nullopt otherwise (so the gizmo shows/hides automatically).
  struct GizmoTarget {
    RoadId road;
    ObjectId object;
    std::array<double, 3> pivot{};
  };

  /// An in-flight gizmo drag: the grabbed handle, the entity, its pivot, and the
  /// press anchors (world + pixels) the constraint math resolves against.
  struct GizmoDrag {
    GizmoHandle handle = GizmoHandle::None;
    RoadId road;
    ObjectId object;
    std::array<double, 3> pivot{};
    std::array<double, 2> press_world{};
    QPoint press_px;
    double base_hdg = 0.0; ///< prop heading at press (for yaw)
    QString summary;       ///< toast text, refreshed each frame
  };

  [[nodiscard]] std::optional<GizmoTarget> gizmo_target() const;

  /// Draws the gizmo (axis arrows, yaw ring, XY pad) when gizmo_target() exists.
  void draw_gizmo(QPainter& painter) const;

  /// Starts a gizmo drag if `pos` is over a handle (returns true and consumes
  /// the press); false lets the press fall through to the active tool.
  [[nodiscard]] bool begin_gizmo_drag(const QPointF& pos);

  /// One gizmo-drag frame: constrains the motion to the grabbed handle and
  /// previews the matching edit command (translate / rotate / elevation).
  void update_gizmo_drag(const QPointF& pos, Qt::KeyboardModifiers modifiers);

  /// Commits the gizmo drag as ONE undo step with a summary toast.
  void commit_gizmo_drag();

  /// Reverts the gizmo drag's live preview (Esc / interruption).
  void cancel_gizmo_drag();

  /// True when the drag carries a library item (accept it as a drop).
  [[nodiscard]] static bool has_library_mime(const QDropEvent* event);

  /// Emits library_item_drag_moved for `event`'s key at the surface point under
  /// the cursor, so MainWindow can resolve the drop and position the ghost.
  void emit_drag_preview_request(const QDropEvent* event);

  /// Hint-card opacity from how long since the hint last changed (1 while
  /// fresh, ramping to 0 after the idle hold).
  [[nodiscard]] double hint_opacity() const;

  /// Elapsed ms since construction — the overlay clock (hint/toast timing).
  [[nodiscard]] std::int64_t now_ms() const;

  /// Runs the overlay repaint timer while anything is animating (a live toast
  /// or a not-yet-faded hint), stopping it when the overlay is static.
  void refresh_overlay_animation();

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

  /// Render mode: false = plain-color + reference-grid (Sober, the DEFAULT),
  /// true = daytime Textured (lit surfaces, grass ground, textures — opt-in).
  /// Drives the renderer Environment/ground/grid via apply_render_mode().
  bool textured_rendering_ = false;

  /// Surface textures uploaded once at GL init (from the :/textures qrc). Invalid
  /// if loading failed — material_for then falls back to the flat mesh color.
  TextureHandle asphalt_texture_;
  TextureHandle concrete_texture_;

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

  /// Navigation chords (⌥ orbit/zoom/pan/pivot, MMB pan, and the legacy RMB
  /// orbit-or-context binding). Consulted BEFORE the gizmo and the active tool
  /// so a chord can never leak into an edit — except while a gizmo drag owns
  /// the mouse, which stays exclusive until its LMB release.
  NavController nav_;

  /// Corner hint text (set_hint); painted over the GL frame in paintGL.
  QString hint_text_;

  /// Transient toast overlay + the shared overlay clock/animation timer. The
  /// hint fades relative to when it last changed.
  ToastQueue toasts_;
  QElapsedTimer clock_;
  QTimer* overlay_timer_ = nullptr;
  std::int64_t hint_changed_ms_ = 0;

  /// Resolved landing point (world x, y) and validity of a library item being
  /// dragged over the viewport; drives the world-anchored drop ghost (a marker
  /// at the spot the item will commit). Empty when no drag is active. Set by
  /// MainWindow, which resolves the drag through the same resolve_library_drop
  /// the drop commit uses (ghost==commit).
  struct DropPreview {
    double x = 0.0;
    double y = 0.0;
    bool valid = false;
  };

  std::optional<DropPreview> drop_preview_;

  /// Active gizmo drag (nullopt when idle) and the handle currently hovered
  /// (for the highlight). See the transform-gizmo methods above.
  std::optional<GizmoDrag> gizmo_drag_;
  GizmoHandle gizmo_hover_ = GizmoHandle::None;

  /// Entity under the cursor, tracked by update_hover for the hover highlight
  /// (invalid = nothing hovered). A lane-level hover sets both; a road-level
  /// hover leaves the lane invalid. Selection (SelectionModel) takes priority
  /// over these when both apply to the same mesh.
  RoadId hovered_road_;
  LaneId hovered_lane_;

  /// The road a road-style drag is currently over (invalid = none). Drives the
  /// same Hover highlight as hovered_road_ but is set from the drag-move handler
  /// (Qt does not deliver mouse-move hover during a drag).
  RoadId drag_target_road_;

  /// Prop/object under the cursor (invalid = none). Mutually exclusive with
  /// the road/lane hover — an object hit clears the road hover and vice versa.
  ObjectId hovered_object_;

  /// Signal under the cursor (invalid = none). Mutually exclusive with the
  /// road/lane/object hover.
  SignalId hovered_signal_;

  /// Junction floor under the cursor (invalid = none). Mutually exclusive with
  /// the road/object hover.
  JunctionId hovered_junction_;

  /// Ground surface under the cursor (invalid = none, #215). Mutually exclusive
  /// with the road/object/junction hover.
  SurfaceId hovered_surface_;

  /// Set by set_hover_preview (screenshot mode): update_hover then leaves the
  /// forced hover in place so a capture isn't wiped by a spurious event.
  bool hover_locked_ = false;
};

} // namespace roadmaker::editor
