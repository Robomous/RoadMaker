#include "viewport/viewport_widget.hpp"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "render/gl_functions.hpp"
#include "render/gl_renderer.hpp"
#include "render/scene_builder.hpp"
#include "theme/theme.hpp"

namespace roadmaker::editor {

namespace {

/// Captureless lambda decays to gl::ProcResolver; keeps render/ Qt-free.
void* qt_gl_resolver(const char* name) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  return context == nullptr ? nullptr : reinterpret_cast<void*>(context->getProcAddress(name));
}

} // namespace

ViewportWidget::ViewportWidget(Document& document,
                               SelectionModel& selection,
                               ToolManager& tools,
                               QWidget* parent)
    : QOpenGLWidget(parent), document_(document), selection_(selection), tools_(tools),
      renderer_(std::make_unique<GLRenderer>()) {
  setMouseTracking(true); // hover s/t without a pressed button
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);
  renderer_->set_backdrop(theme::current().backdrop()); // safe pre-init

  connect(&document_, &Document::loaded, this, [this] { frame_on_rebuild_ = true; });
  connect(&document_, &Document::mesh_changed, this, [this](const std::vector<RoadId>& roads) {
    if (roads.empty()) { // everything changed — full rebuild
      scene_dirty_ = true;
      pending_roads_.clear();
      road_aabbs_ = compute_road_aabbs(document_.mesh());
    } else if (!scene_dirty_) { // partial path; a pending full rebuild covers it
      pending_roads_.insert(pending_roads_.end(), roads.begin(), roads.end());
      refresh_road_aabbs(roads);
    }
    update();
  });
  connect(&selection_, &SelectionModel::selection_changed, this, [this] { update(); });
  connect(&tools_, &ToolManager::active_changed, this, &ViewportWidget::attach_active_tool);
  attach_active_tool();
}

ViewportWidget::~ViewportWidget() {
  // GL resources must die while the context is current (QOpenGLWidget rule).
  makeCurrent();
  if (gl_ready_) {
    renderer_->shutdown();
  }
  doneCurrent();
}

void ViewportWidget::initializeGL() {
  gl_ready_ = gl::load_functions(&qt_gl_resolver) && renderer_->init();
  if (!gl_ready_) {
    qFatal("Failed to initialize OpenGL 3.3 core renderer");
  }
  scene_dirty_ = true;
}

void ViewportWidget::rebuild_scene() {
  renderer_->clear_meshes();
  items_.clear();
  pending_roads_.clear();
  preview_handles_.clear(); // clear_meshes() dropped them; re-uploaded this paint

  Scene scene = build_scene(document_.mesh());
  items_.reserve(scene.items.size());
  for (const SceneItem& item : scene.items) {
    items_.push_back(
        UploadedItem{.handle = renderer_->upload(item.data), .road = item.road, .lane = item.lane});
  }
  scene_bounds_ = scene.bounds;
  if (scene_bounds_.valid() && frame_on_rebuild_) {
    camera_.frame(scene_bounds_.center(), scene_bounds_.framing_radius());
  }
  frame_on_rebuild_ = false;
  scene_dirty_ = false;
}

void ViewportWidget::apply_pending_road_updates() {
  std::ranges::sort(pending_roads_, [](RoadId a, RoadId b) {
    return a.index != b.index ? a.index < b.index : a.gen < b.gen;
  });
  const auto duplicates = std::ranges::unique(pending_roads_);
  pending_roads_.erase(duplicates.begin(), duplicates.end());

  for (const RoadId road_id : pending_roads_) {
    // Drop the road's previous GPU meshes and items...
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->road == road_id) {
        renderer_->remove(it->handle);
        it = items_.erase(it);
      } else {
        ++it;
      }
    }
    // ...and upload its current tessellation (absent = road stayed erased).
    for (const RoadMesh& road : document_.mesh().roads) {
      if (road.road != road_id) {
        continue;
      }
      Scene scene;
      append_road_items(road, scene);
      for (const SceneItem& item : scene.items) {
        items_.push_back(UploadedItem{
            .handle = renderer_->upload(item.data), .road = item.road, .lane = item.lane});
      }
      break;
    }
  }
  pending_roads_.clear();
}

void ViewportWidget::refresh_road_aabbs(const std::vector<RoadId>& roads) {
  const NetworkMesh& mesh = document_.mesh();
  if (road_aabbs_.size() != mesh.roads.size()) { // topology drifted — resync
    road_aabbs_ = compute_road_aabbs(mesh);
    return;
  }
  for (const RoadId road_id : roads) {
    for (std::size_t i = 0; i < mesh.roads.size(); ++i) {
      if (mesh.roads[i].road == road_id) {
        road_aabbs_[i] = compute_road_aabb(mesh.roads[i]);
        break;
      }
    }
  }
}

void ViewportWidget::set_camera_preset(const QString& preset) {
  constexpr float kPi = 3.14159265358979F;
  if (preset == QStringLiteral("top")) {
    // Pitch just under vertical: the look-at up vector stays well-defined
    // and the plan view keeps a hint of depth for seam inspection.
    camera_.set_view(-kPi / 2.0F, (kPi / 2.0F) - 0.02F);
  } else if (preset == QStringLiteral("orbit")) {
    camera_.set_view(0.8F, 0.9F);
  }
  update();
}

QImage ViewportWidget::capture_frame() {
  if (!gl_ready_) {
    return {};
  }
  return grabFramebuffer(); // makes the context current and re-renders
}

bool ViewportWidget::is_highlighted(const UploadedItem& item) const {
  for (const SelectionEntry& entry : selection_.entries()) {
    if (entry.lane.is_valid() ? item.lane == entry.lane : item.road == entry.road) {
      return true;
    }
  }
  return false;
}

void ViewportWidget::paintGL() {
  if (scene_dirty_) {
    rebuild_scene();
  } else if (!pending_roads_.empty()) {
    apply_pending_road_updates();
  }
  upload_tool_preview();

  std::vector<DrawItem> draw_items;
  draw_items.reserve(items_.size() + preview_handles_.size());
  for (const UploadedItem& item : items_) {
    draw_items.push_back(DrawItem{.mesh = item.handle, .highlighted = is_highlighted(item)});
  }
  for (const RenderMeshHandle handle : preview_handles_) {
    draw_items.push_back(DrawItem{.mesh = handle});
  }

  // Framebuffer pixels, not widget units (HiDPI rule).
  const auto fb_width = static_cast<int>(std::lround(width() * devicePixelRatioF()));
  const auto fb_height = static_cast<int>(std::lround(height() * devicePixelRatioF()));
  const float aspect =
      fb_height > 0 ? static_cast<float>(fb_width) / static_cast<float>(fb_height) : 1.0F;
  renderer_->render(draw_items, camera_.matrices(aspect), fb_width, fb_height);

  // Active-tool hint in the viewport corner (QPainter-over-GL is supported
  // on QOpenGLWidget; beginNativePainting is not needed as we paint last).
  if (!hint_text_.isEmpty()) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QFontMetrics metrics(painter.font());
    const int pad = 6;
    const QRect text_rect =
        metrics.boundingRect(QRect(0, 0, width() - (4 * pad), 0), Qt::TextWordWrap, hint_text_);
    const QRect box(pad,
                    height() - text_rect.height() - (3 * pad),
                    text_rect.width() + (2 * pad),
                    text_rect.height() + (2 * pad));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(box.adjusted(pad, pad, -pad, -pad), Qt::TextWordWrap, hint_text_);
  }
}

void ViewportWidget::set_hint(const QString& text) {
  if (hint_text_ == text) {
    return;
  }
  hint_text_ = text;
  update();
}

Ray ViewportWidget::ray_through(const QPointF& pos) const {
  // make_pick_ray works in any self-consistent pixel space; widget units and
  // the widget-unit viewport size cancel the DPR out.
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  return make_pick_ray(camera_.matrices(aspect),
                       pos.x(),
                       pos.y(),
                       static_cast<double>(width()),
                       static_cast<double>(height()));
}

std::optional<std::array<double, 3>> ViewportWidget::ground_point_at(const QPointF& pos,
                                                                     double max_t) const {
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  return ground_point(camera_.matrices(aspect),
                      pos.x(),
                      pos.y(),
                      static_cast<double>(width()),
                      static_cast<double>(height()),
                      max_t);
}

void ViewportWidget::update_hover(const QPointF& pos) {
  HoverInfo info;
  const Ray ray = ray_through(pos);

  // Cursor world x/y: ray ∩ ground plane (z = 0).
  if (const auto ground = ground_point_at(pos)) {
    info.valid = true;
    info.world_x = (*ground)[0];
    info.world_y = (*ground)[1];
  }

  if (const auto hit = pick(document_.mesh(), road_aabbs_, ray)) {
    const Road* road = document_.network().road(hit->road);
    const Lane* lane = document_.network().lane(hit->lane);
    if (road != nullptr) {
      info.valid = true;
      info.on_road = true;
      info.world_x = hit->position[0];
      info.world_y = hit->position[1];
      info.entity =
          lane == nullptr
              ? tr("road %1").arg(QString::fromStdString(road->odr_id))
              : tr("road %1 / lane %2").arg(QString::fromStdString(road->odr_id)).arg(lane->odr_id);
      const StationCoord coord = find_station(road->plan_view, hit->position[0], hit->position[1]);
      info.s = coord.s;
      info.t = coord.t;
    }
  }
  emit hover_changed(info);
}

// M2 button map (docs/design/m2/01_editing_framework.md §4): LMB drives the
// active tool (click-select, rubber band, node drag live in SelectTool),
// RMB-drag orbits, MMB-drag pans, wheel zooms.

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->pos();
  if (event->button() == Qt::MiddleButton) {
    // Grab the ground point under the cursor; the anchored pan keeps it pinned.
    // Cap the ray so a near-horizon grab at low pitch falls back to view-plane.
    pan_anchor_world_ = ground_point_at(event->position(), 10.0 * camera_.distance());
  }
  if (Tool* tool = tools_.active(); tool != nullptr && event->button() == Qt::LeftButton &&
                                    tool->mouse_press(make_tool_event(event))) {
    update();
  }
  event->accept();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
  const QPoint delta = event->pos() - last_mouse_pos_;
  last_mouse_pos_ = event->pos();

  if (Tool* tool = tools_.active(); tool != nullptr && tool->mouse_move(make_tool_event(event))) {
    update();
    event->accept();
    return;
  }

  if ((event->buttons() & Qt::RightButton) != 0) {
    camera_.orbit(static_cast<float>(-delta.x()) * 0.008F, static_cast<float>(delta.y()) * 0.008F);
    update();
  } else if ((event->buttons() & Qt::MiddleButton) != 0) {
    // Ground-anchored pan: keep the grabbed world point under the cursor at 1:1
    // (RoadRunner/CAD/maps feel, zero tuning constants). Degenerate near-horizon
    // rays (no anchor, or the current ray misses / is capped) fall back to a
    // correctly-scaled view-plane pan.
    const std::optional<std::array<double, 3>> current =
        ground_point_at(event->position(), 10.0 * camera_.distance());
    if (pan_anchor_world_ && current) {
      camera_.move_target(static_cast<float>((*pan_anchor_world_)[0] - (*current)[0]),
                          static_cast<float>((*pan_anchor_world_)[1] - (*current)[1]));
    } else {
      camera_.pan_pixels(static_cast<float>(delta.x()),
                         static_cast<float>(delta.y()),
                         static_cast<float>(height()));
    }
    update();
  } else {
    update_hover(event->position());
  }
  event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) {
    pan_anchor_world_.reset();
  }
  if (Tool* tool = tools_.active(); tool != nullptr && event->button() == Qt::LeftButton &&
                                    tool->mouse_release(make_tool_event(event))) {
    update();
  }
  event->accept();
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  if (Tool* tool = tools_.active(); tool != nullptr && event->button() == Qt::LeftButton &&
                                    tool->mouse_double_click(make_tool_event(event))) {
    update();
  }
  event->accept();
}

void ViewportWidget::keyPressEvent(QKeyEvent* event) {
  if (Tool* tool = tools_.active();
      tool != nullptr && tool->key_press(event->key(), event->modifiers())) {
    update();
    event->accept();
    return;
  }
  QOpenGLWidget::keyPressEvent(event);
}

ToolEvent ViewportWidget::make_tool_event(const QMouseEvent* event) const {
  ToolEvent tool_event;
  const Ray ray = ray_through(event->position());
  // Cursor world x/y: ray ∩ ground plane (z = 0), like the hover readout.
  if (const auto ground = ground_point_at(event->position())) {
    tool_event.world_x = (*ground)[0];
    tool_event.world_y = (*ground)[1];
  }
  tool_event.pick = pick(document_.mesh(), road_aabbs_, ray);
  tool_event.buttons = event->buttons(); // press events include their button
  tool_event.modifiers = event->modifiers();
  return tool_event;
}

void ViewportWidget::attach_active_tool() {
  disconnect(preview_connection_);
  disconnect(cursor_connection_);
  if (Tool* tool = tools_.active()) {
    preview_connection_ =
        connect(tool, &Tool::preview_changed, this, qOverload<>(&QWidget::update));
    cursor_connection_ = connect(
        tool, &Tool::cursor_changed, this, [this](Qt::CursorShape shape) { setCursor(shape); });
  } else {
    setCursor(Qt::ArrowCursor);
  }
  update();
}

void ViewportWidget::upload_tool_preview() {
  for (const RenderMeshHandle handle : preview_handles_) {
    renderer_->remove(handle);
  }
  preview_handles_.clear();

  const Tool* tool = tools_.active();
  if (tool == nullptr) {
    return;
  }
  const PreviewGeometry geometry = tool->preview();
  if (geometry.empty()) {
    return;
  }

  // Accent-colored overlay (theme token, ui-design.md), lifted slightly off
  // the ground plane so handles and band lines never z-fight the road
  // surface.
  const QColor accent = theme::current().accent;
  const std::array<float, 4> kOverlayColor{static_cast<float>(accent.redF()),
                                           static_cast<float>(accent.greenF()),
                                           static_cast<float>(accent.blueF()),
                                           1.0F};
  constexpr float kOverlayLift = 0.05F;

  if (!geometry.line_positions.empty()) {
    RenderMeshData lines;
    lines.kind = PrimitiveKind::Lines;
    lines.color = kOverlayColor;
    lines.positions.reserve(geometry.line_positions.size());
    for (std::size_t i = 0; i < geometry.line_positions.size(); ++i) {
      const float lift = i % 3 == 2 ? kOverlayLift : 0.0F;
      lines.positions.push_back(static_cast<float>(geometry.line_positions[i]) + lift);
    }
    lines.indices.resize(lines.positions.size() / 3);
    std::iota(lines.indices.begin(), lines.indices.end(), 0U);
    preview_handles_.push_back(renderer_->upload(lines));
  }

  if (!geometry.point_positions.empty()) {
    // The renderer has no point primitive: draw each point as a 3-axis cross
    // sized relative to the view distance (~a steady few pixels on screen).
    const float size = std::max(camera_.distance() * 0.012F, 0.05F);
    RenderMeshData points;
    points.kind = PrimitiveKind::Lines;
    points.color = kOverlayColor;
    points.positions.reserve(geometry.point_positions.size() * 6);
    for (std::size_t i = 0; i + 2 < geometry.point_positions.size(); i += 3) {
      const auto x = static_cast<float>(geometry.point_positions[i]);
      const auto y = static_cast<float>(geometry.point_positions[i + 1]);
      const auto z = static_cast<float>(geometry.point_positions[i + 2]) + kOverlayLift;
      points.positions.insert(points.positions.end(),
                              {x - size,
                               y,
                               z,
                               x + size,
                               y,
                               z, // x arm
                               x,
                               y - size,
                               z,
                               x,
                               y + size,
                               z, // y arm
                               x,
                               y,
                               z - size,
                               x,
                               y,
                               z + size}); // z arm
    }
    points.indices.resize(points.positions.size() / 3);
    std::iota(points.indices.begin(), points.indices.end(), 0U);
    preview_handles_.push_back(renderer_->upload(points));
  }
}

void ViewportWidget::wheelEvent(QWheelEvent* event) {
  const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
  if (steps != 0.0F) {
    camera_.zoom(steps);
    update();
  }
  event->accept();
}

void ViewportWidget::reset_camera() {
  camera_ = OrbitCamera{};
  if (scene_bounds_.valid()) {
    camera_.frame(scene_bounds_.center(), scene_bounds_.framing_radius());
  }
  update();
}

void ViewportWidget::frame_selection() {
  if (selection_.empty() || !scene_bounds_.valid()) {
    if (scene_bounds_.valid()) {
      camera_.frame(scene_bounds_.center(), scene_bounds_.framing_radius());
      update();
    }
    return;
  }

  // Bounds of every selected road's uploaded meshes, recomputed from the
  // kernel mesh — cheap at selection frequency.
  NetworkMesh selected;
  for (const RoadMesh& road : document_.mesh().roads) {
    const auto& entries = selection_.entries();
    const bool road_selected = std::ranges::any_of(
        entries, [&](const SelectionEntry& entry) { return entry.road == road.road; });
    if (road_selected) {
      selected.roads.push_back(road);
    }
  }
  const SceneBounds bounds = build_scene(selected).bounds;
  if (bounds.valid()) {
    camera_.frame(bounds.center(), bounds.framing_radius());
    update();
  }
}

} // namespace roadmaker::editor
