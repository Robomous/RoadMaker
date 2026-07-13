#include "viewport/viewport_widget.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "document/highlight.hpp"
#include "render/gl_functions.hpp"
#include "render/gl_renderer.hpp"
#include "render/scene_builder.hpp"
#include "theme/theme.hpp"
#include "viewport/projection.hpp"

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

  // Overlay clock + a repaint timer that only runs while a toast or the hint
  // is animating (started/stopped by refresh_overlay_animation).
  clock_.start();
  overlay_timer_ = new QTimer(this);
  overlay_timer_->setInterval(60);
  connect(overlay_timer_, &QTimer::timeout, this, [this] {
    update();
    refresh_overlay_animation();
  });

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

HighlightState ViewportWidget::item_state(const UploadedItem& item) const {
  return highlight_state_for(
      item.road, item.lane, selection_.entries(), hovered_road_, hovered_lane_);
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
    draw_items.push_back(DrawItem{.mesh = item.handle, .state = item_state(item)});
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

  // QPainter overlays (handle sprites + the tool hint), painted over the GL
  // frame — supported on QOpenGLWidget when done last (no beginNativePainting).
  const bool has_toasts = !toasts_.active(now_ms()).empty();
  if (!handle_overlays_.empty() || !hint_text_.isEmpty() || has_toasts) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    draw_handles(painter);
    draw_hint_card(painter);
    draw_toasts(painter);
  }
}

void ViewportWidget::draw_hint_card(QPainter& painter) const {
  const double opacity = hint_opacity();
  if (hint_text_.isEmpty() || opacity <= 0.0) {
    return;
  }
  const Theme& theme = theme::current();
  constexpr int kPad = 10;    // ui-design.md spacing scale
  constexpr int kRadius = 8;  // card radius
  constexpr int kMargin = 12; // from the viewport edge

  const QFontMetrics metrics(painter.font());
  const int max_text = std::max(120, (width() / 2) - (2 * kPad) - kMargin);
  const QRect text_rect =
      metrics.boundingRect(QRect(0, 0, max_text, 0), Qt::TextWordWrap, hint_text_);
  const QRect box(
      kMargin, kMargin, text_rect.width() + (2 * kPad), text_rect.height() + (2 * kPad));

  painter.save();
  painter.setOpacity(opacity);
  painter.setPen(QPen(theme.border, 1));
  painter.setBrush(theme.bg2);
  painter.drawRoundedRect(box, kRadius, kRadius);
  painter.setPen(theme.text_primary);
  painter.drawText(box.adjusted(kPad, kPad, -kPad, -kPad), Qt::TextWordWrap, hint_text_);
  painter.restore();
}

void ViewportWidget::draw_toasts(QPainter& painter) {
  const std::vector<ToastQueue::Active> toasts = toasts_.active(now_ms());
  if (toasts.empty()) {
    return;
  }
  const Theme& theme = theme::current();
  const auto accent_for = [&](ToastSeverity severity) {
    switch (severity) {
    case ToastSeverity::Success:
      return theme.success;
    case ToastSeverity::Warning:
      return theme.warning;
    case ToastSeverity::Error:
      return theme.error;
    case ToastSeverity::Info:
      break;
    }
    return theme.accent;
  };

  constexpr int kPad = 10;
  constexpr int kRadius = 8;
  constexpr int kGap = 8;
  constexpr int kBar = 3; // severity color bar on the leading edge
  const QFontMetrics metrics(painter.font());

  // Bottom-center stack, newest at the bottom and older ones rising above —
  // clear of the top-left hint card.
  int bottom = height() - 14;
  for (auto toast = toasts.rbegin(); toast != toasts.rend(); ++toast) {
    const int max_text = std::min(420, width() - (4 * kPad));
    const QRect text_rect =
        metrics.boundingRect(QRect(0, 0, max_text, 0), Qt::TextWordWrap, toast->text);
    const int box_w = text_rect.width() + (2 * kPad) + kBar;
    const int box_h = text_rect.height() + (2 * kPad);
    const QRect box((width() - box_w) / 2, bottom - box_h, box_w, box_h);

    painter.save();
    painter.setOpacity(toast->opacity);
    painter.setPen(QPen(theme.border, 1));
    painter.setBrush(theme.bg2);
    painter.drawRoundedRect(box, kRadius, kRadius);
    // Severity color bar on the left edge, clipped to the rounded corners.
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent_for(toast->severity));
    painter.setClipRect(box.left(), box.top(), kBar + kRadius, box.height());
    painter.drawRoundedRect(box, kRadius, kRadius);
    painter.setClipping(false);
    painter.setPen(theme.text_primary);
    painter.drawText(box.adjusted(kPad + kBar, kPad, -kPad, -kPad), Qt::TextWordWrap, toast->text);
    painter.restore();

    bottom -= box_h + kGap;
  }
}

void ViewportWidget::draw_handles(QPainter& painter) const {
  if (handle_overlays_.empty()) {
    return;
  }
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  const CameraMatrices camera = camera_.matrices(aspect);
  const Theme& theme = theme::current();

  for (const Handle& handle : handle_overlays_) {
    const auto screen = project_to_screen(camera,
                                          handle.x,
                                          handle.y,
                                          handle.z,
                                          static_cast<double>(width()),
                                          static_cast<double>(height()));
    if (!screen.has_value()) {
      continue; // behind the camera
    }
    const QPointF center{(*screen)[0], (*screen)[1]};

    // Screen-constant sizes (logical px → DPI-crisp): idle < hovered < grabbed.
    // A light idle dot with a dark outline reads on both the dark ground and a
    // selected (accent) road; hover/grabbed switch the fill to the accent.
    double radius = 4.0;
    QColor fill = theme.text_primary;
    QColor stroke = theme.bg0;
    double stroke_width = 1.5;
    switch (handle.state) {
    case HandleState::Hovered:
      radius = 5.0;
      fill = theme.accent;
      stroke = theme.bg0;
      break;
    case HandleState::Grabbed:
      radius = 6.0;
      fill = theme.accent;
      stroke = theme.text_primary;
      stroke_width = 2.0;
      break;
    case HandleState::Idle:
      break;
    }

    if (handle.kind == HandleKind::Midpoint) {
      // Hollow "insert here" marker: an accent ring with a plus, no fill.
      const double r = 4.0;
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(theme.accent, 1.5));
      painter.drawEllipse(center, r, r);
      painter.drawLine(QPointF(center.x() - (r * 0.55), center.y()),
                       QPointF(center.x() + (r * 0.55), center.y()));
      painter.drawLine(QPointF(center.x(), center.y() - (r * 0.55)),
                       QPointF(center.x(), center.y() + (r * 0.55)));
      continue;
    }

    painter.setBrush(fill);
    painter.setPen(QPen(stroke, stroke_width));
    painter.drawEllipse(center, radius, radius);
  }
}

void ViewportWidget::set_hint(const QString& text) {
  if (hint_text_ == text) {
    return;
  }
  hint_text_ = text;
  hint_changed_ms_ = now_ms(); // reset the idle-fade clock
  refresh_overlay_animation();
  update();
}

void ViewportWidget::show_toast(const QString& text, ToastSeverity severity) {
  if (text.isEmpty()) {
    return;
  }
  toasts_.push(text, severity, now_ms());
  refresh_overlay_animation();
  update();
}

std::int64_t ViewportWidget::now_ms() const {
  return clock_.isValid() ? clock_.elapsed() : 0;
}

double ViewportWidget::hint_opacity() const {
  constexpr std::int64_t kHoldMs = 4000; // full opacity before the idle fade
  constexpr std::int64_t kFadeMs = 700;
  const std::int64_t age = now_ms() - hint_changed_ms_;
  if (age <= kHoldMs) {
    return 1.0;
  }
  if (age >= kHoldMs + kFadeMs) {
    return 0.0;
  }
  return static_cast<double>(kHoldMs + kFadeMs - age) / static_cast<double>(kFadeMs);
}

void ViewportWidget::refresh_overlay_animation() {
  if (overlay_timer_ == nullptr) {
    return;
  }
  const bool animating =
      !toasts_.active(now_ms()).empty() || (!hint_text_.isEmpty() && hint_opacity() > 0.0);
  if (animating && !overlay_timer_->isActive()) {
    overlay_timer_->start();
  } else if (!animating && overlay_timer_->isActive()) {
    overlay_timer_->stop();
  }
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

  RoadId new_hover_road; // invalid = nothing under the cursor
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
      new_hover_road = hit->road;
    }
  }
  // Highlight the whole hovered road (a road-level hover: lane left invalid);
  // repaint only when it actually changes so plain mouse-overs stay cheap.
  set_hovered_road(new_hover_road);
  emit hover_changed(info);
}

void ViewportWidget::set_hovered_road(RoadId road) {
  // A capture forced the hover (set_hover_preview): ignore live enter/leave/
  // move updates so the screenshot state holds. Interactive sessions never
  // lock, so this is a no-op there.
  if (hover_locked_) {
    return;
  }
  if (hovered_road_ == road && !hovered_lane_.is_valid()) {
    return;
  }
  hovered_road_ = road;
  hovered_lane_ = {};
  update();
}

void ViewportWidget::leaveEvent(QEvent* event) {
  set_hovered_road({}); // cursor left the viewport — drop the hover highlight
  QOpenGLWidget::leaveEvent(event);
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
  if (event->button() == Qt::RightButton) {
    // RMB is orbit-or-context: remember where it went down; a drag past the
    // threshold orbits, a release without one pops the context menu.
    rmb_press_pos_ = event->pos();
    rmb_orbiting_ = false;
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
    // Only orbit once the drag passes the click threshold — a small wiggle
    // still opens the context menu on release (platform-timing-safe vs
    // contextMenuEvent).
    if (!rmb_orbiting_ && (event->pos() - rmb_press_pos_).manhattanLength() >
                              static_cast<int>(std::lround(4.0 * devicePixelRatioF()))) {
      rmb_orbiting_ = true;
    }
    if (rmb_orbiting_) {
      camera_.orbit(static_cast<float>(-delta.x()) * 0.008F,
                    static_cast<float>(delta.y()) * 0.008F);
      update();
    }
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
  if (event->button() == Qt::RightButton && !rmb_orbiting_) {
    emit context_menu_requested(build_menu_context(event->position()),
                                event->globalPosition().toPoint());
    event->accept();
    return;
  }
  if (Tool* tool = tools_.active(); tool != nullptr && event->button() == Qt::LeftButton &&
                                    tool->mouse_release(make_tool_event(event))) {
    update();
  }
  event->accept();
}

MenuContext ViewportWidget::build_menu_context(const QPointF& pos) const {
  MenuContext context;
  const Ray ray = ray_through(pos);

  // World point on the ground plane, for node picking.
  if (std::abs(ray.direction[2]) > 1e-12) {
    const double t = -ray.origin[2] / ray.direction[2];
    if (t >= 0.0) {
      const double wx = ray.origin[0] + (ray.direction[0] * t);
      const double wy = ray.origin[1] + (ray.direction[1] * t);
      // A node handle of a selected road wins over the body.
      if (auto node =
              pick_waypoint(document_.network(), selection_.selected_roads(), wx, wy, 2.0)) {
        context.node = node;
      }
    }
  }

  if (const auto hit = pick(document_.mesh(), road_aabbs_, ray)) {
    context.pick = hit;
    if (const Road* road = document_.network().road(hit->road)) {
      context.station = find_station(road->plan_view, hit->position[0], hit->position[1]).s;
    }
  }
  return context;
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
  handle_overlays_.clear();

  const Tool* tool = tools_.active();
  if (tool == nullptr) {
    return;
  }
  const PreviewGeometry geometry = tool->preview();
  if (geometry.empty()) {
    return;
  }

  // Line overlays (tangent/drag whiskers, band lines) go to GL in the accent
  // token, lifted slightly off the ground so they never z-fight the surface.
  if (!geometry.line_positions.empty()) {
    const QColor accent = theme::current().accent;
    constexpr float kOverlayLift = 0.05F;
    RenderMeshData lines;
    lines.kind = PrimitiveKind::Lines;
    lines.color = {static_cast<float>(accent.redF()),
                   static_cast<float>(accent.greenF()),
                   static_cast<float>(accent.blueF()),
                   1.0F};
    lines.positions.reserve(geometry.line_positions.size());
    for (std::size_t i = 0; i < geometry.line_positions.size(); ++i) {
      const float lift = i % 3 == 2 ? kOverlayLift : 0.0F;
      lines.positions.push_back(static_cast<float>(geometry.line_positions[i]) + lift);
    }
    lines.indices.resize(lines.positions.size() / 3);
    std::iota(lines.indices.begin(), lines.indices.end(), 0U);
    preview_handles_.push_back(renderer_->upload(lines));
  }

  // Handle knobs are drawn as screen-space QPainter sprites in draw_handles()
  // (screen-constant size, DPI-crisp) rather than world-meter GL crosses.
  handle_overlays_ = geometry.handles;
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
