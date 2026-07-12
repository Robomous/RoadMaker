#pragma once

// The 3D viewport: a QOpenGLWidget (3.3 core, requested app-wide via
// QSurfaceFormat in main()) hosting the toolkit-agnostic GLRenderer. Owns the
// camera and the picking state; selection flows OUT through SelectionModel
// and highlight state flows back IN through it — never widget-to-widget.

#include <QImage>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <memory>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "render/renderer.hpp"
#include "render/scene_builder.hpp"
#include "tools/tool_manager.hpp"
#include "viewport/camera.hpp"
#include "viewport/picking.hpp"

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

  /// Renders the current scene into an offscreen framebuffer and returns the
  /// frame (screenshot mode, docs/contributing/pull-requests.md visual
  /// evidence). Runs any pending scene upload first via paintGL. Null image
  /// when GL never initialized.
  [[nodiscard]] QImage capture_frame();

signals:
  void hover_changed(const roadmaker::editor::HoverInfo& info);

protected:
  void initializeGL() override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

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

  [[nodiscard]] bool is_highlighted(const UploadedItem& item) const;
  [[nodiscard]] Ray ray_through(const QPointF& pos) const;
  void update_hover(const QPointF& pos);

  /// Translates a Qt mouse event into the tool seam's abstract event:
  /// ground-plane (z=0) world position, lane-patch pick, buttons, modifiers.
  [[nodiscard]] ToolEvent make_tool_event(const QMouseEvent* event) const;

  /// Re-subscribes preview_changed to the newly active tool.
  void attach_active_tool();

  /// Replaces the overlay meshes with the active tool's PreviewGeometry
  /// (GL context must be current — called from paintGL).
  void upload_tool_preview();

  Document& document_;
  SelectionModel& selection_;
  ToolManager& tools_;

  std::unique_ptr<Renderer> renderer_;
  OrbitCamera camera_;
  std::vector<UploadedItem> items_;
  RenderMeshHandle grid_;
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

  /// Overlay meshes for the active tool's preview, replaced every paint.
  std::vector<RenderMeshHandle> preview_handles_;
  QMetaObject::Connection preview_connection_;

  QPoint last_mouse_pos_;

  /// Corner hint text (set_hint); painted over the GL frame in paintGL.
  QString hint_text_;
};

} // namespace roadmaker::editor
