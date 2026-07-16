#include "viewport/viewport_widget.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <numeric>

#include "document/highlight.hpp"
#include "document/library_list_model.hpp"
#include "render/gl_functions.hpp"
#include "render/gl_renderer.hpp"
#include "render/scene_builder.hpp"
#include "theme/theme.hpp"
#include "viewport/framing.hpp"
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
  setAcceptDrops(true); // library items drag onto the scene to create geometry
  apply_render_mode();  // pushes backdrop + environment (both safe pre-init)

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

  // Upload the surface textures once (bundled in the :/textures qrc). A failed
  // load leaves an invalid handle and material_for() falls back to flat color,
  // so a runner without the JPEG image handler still renders (untextured).
  const auto upload_texture = [this](const char* resource) -> TextureHandle {
    QImage image(QString::fromLatin1(resource));
    if (image.isNull()) {
      qWarning("viewport: could not load surface texture %s", resource);
      return {};
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    TextureData tex;
    tex.width = image.width();
    tex.height = image.height();
    tex.rgba.resize(static_cast<std::size_t>(image.width()) *
                    static_cast<std::size_t>(image.height()) * 4U);
    // Copy scanline by scanline (QImage rows may be padded to 4-byte alignment).
    const std::size_t row_bytes = static_cast<std::size_t>(image.width()) * 4U;
    for (int y = 0; y < image.height(); ++y) {
      std::memcpy(tex.rgba.data() + (static_cast<std::size_t>(y) * row_bytes),
                  image.constScanLine(y),
                  row_bytes);
    }
    return renderer_->upload(tex);
  };
  asphalt_texture_ = upload_texture(":/textures/asphalt.jpg");
  concrete_texture_ = upload_texture(":/textures/concrete.jpg");

  scene_dirty_ = true;
}

Material ViewportWidget::material_for(SurfaceKind surface) const {
  Material material;
  if (!textured_rendering_) {
    return material; // Sober mode: flat per-mesh color, no textures/paint.
  }
  switch (surface) {
  case SurfaceKind::Asphalt:
    material.base_color = asphalt_texture_; // invalid → renderer uses mesh color
    break;
  case SurfaceKind::Concrete:
    material.base_color = concrete_texture_;
    break;
  case SurfaceKind::Paint:
    material.unlit = true; // markings read as bright flat paint, not shaded
    break;
  case SurfaceKind::Untextured:
    break;
  }
  return material;
}

void ViewportWidget::rebuild_scene() {
  renderer_->clear_meshes();
  items_.clear();
  pending_roads_.clear();
  preview_handles_.clear(); // clear_meshes() dropped them; re-uploaded this paint

  Scene scene = build_scene(document_.mesh());
  items_.reserve(scene.items.size());
  for (const SceneItem& item : scene.items) {
    items_.push_back(UploadedItem{.handle = renderer_->upload(item.data),
                                  .road = item.road,
                                  .lane = item.lane,
                                  .object = item.object,
                                  .signal = item.signal,
                                  .junction = item.junction,
                                  .surface = item.surface});
  }
  scene_bounds_ = scene.bounds;
  // Keep the ground plane just under the (possibly changed) network floor.
  renderer_->set_ground(textured_rendering_, ground_base_z(scene_bounds_));
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
    // Drop the road's previous surface meshes and items — but NOT its prop
    // items (props re-upload on the objects channel; a road-geometry edit
    // leaves them in place).
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->road == road_id && !it->object.is_valid()) {
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
        items_.push_back(UploadedItem{.handle = renderer_->upload(item.data),
                                      .road = item.road,
                                      .lane = item.lane,
                                      .object = item.object,
                                      .surface = item.surface});
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
  } else if (preset == QStringLiteral("ortho")) {
    // Orthographic plan view: the true parallel projection (no foreshortening),
    // so a capture can show what the O toggle actually does.
    camera_.set_view(-kPi / 2.0F, OrbitCamera::kTopDownPitch);
    camera_.set_projection(ProjectionMode::Orthographic);
  } else if (preset == QStringLiteral("orbit")) {
    camera_.set_view(0.8F, 0.9F);
  } else if (preset == QStringLiteral("gs1")) {
    // The GS-1 golden camera (docs/roadmap/golden_scenes/gs1_urban_intersection.md):
    // a fixed three-quarter view down one diagonal at eye (−55, −55, 35) looking
    // at the junction centre (origin). yaw = −3π/4, pitch = asin(35/|d|), and the
    // exact distance |(−55,−55,35)| = 85.29 m so the baseline is reproducible
    // regardless of the loaded scene's bounds.
    camera_.set_pose({0.0F, 0.0F, 0.0F}, -3.0F * kPi / 4.0F, 0.42294F, 85.294F);
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
  return highlight_state_for(item.road,
                             item.lane,
                             item.object,
                             item.signal,
                             item.junction,
                             selection_.entries(),
                             hovered_road_,
                             hovered_lane_,
                             hovered_object_,
                             hovered_signal_,
                             hovered_junction_);
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
    draw_items.push_back(DrawItem{
        .mesh = item.handle, .state = item_state(item), .material = material_for(item.surface)});
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
  const bool has_gizmo = gizmo_target().has_value();
  if (!handle_overlays_.empty() || !hint_text_.isEmpty() || has_toasts ||
      drop_preview_.has_value() || has_gizmo) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    draw_handles(painter);
    draw_gizmo(painter);
    draw_drag_ghost(painter);
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

std::optional<std::array<double, 3>> ViewportWidget::drop_world_point(const QPointF& pos) const {
  // Same projection as the hover readout: the real surface under the cursor
  // (road/junction/prop) via pick(), falling back to the ground plane in empty
  // space — never a hidden z=0 plane over an elevated surface.
  const Ray ray = ray_through(pos);
  if (const auto hit = pick(document_.mesh(), road_aabbs_, ray)) {
    return hit->position;
  }
  return ground_point_at(pos);
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

  RoadId new_hover_road;         // invalid = no road under the cursor
  ObjectId new_hover_object;     // invalid = no prop under the cursor
  SignalId new_hover_signal;     // invalid = no signal under the cursor
  JunctionId new_hover_junction; // invalid = no junction floor under the cursor
  if (const auto hit = pick(document_.mesh(), road_aabbs_, ray)) {
    if (hit->junction.is_valid()) {
      if (const Junction* junction = document_.network().junction(hit->junction)) {
        info.valid = true;
        info.world_x = hit->position[0];
        info.world_y = hit->position[1];
        info.entity = tr("junction %1").arg(QString::fromStdString(junction->odr_id));
        new_hover_junction = hit->junction;
      }
    } else if (hit->object.is_valid()) {
      // A prop is nearer than any road surface — highlight the whole tree.
      if (const Object* object = document_.network().object(hit->object)) {
        info.valid = true;
        info.world_x = hit->position[0];
        info.world_y = hit->position[1];
        info.entity = tr("object %1").arg(QString::fromStdString(object->odr_id));
        new_hover_object = hit->object;
      }
    } else if (hit->signal.is_valid()) {
      // A signal is nearer than any road surface — highlight the whole pole.
      if (const Signal* signal = document_.network().signal(hit->signal)) {
        info.valid = true;
        info.world_x = hit->position[0];
        info.world_y = hit->position[1];
        info.entity = tr("signal %1").arg(QString::fromStdString(signal->odr_id));
        new_hover_signal = hit->signal;
      }
    } else if (const Road* road = document_.network().road(hit->road)) {
      const Lane* lane = document_.network().lane(hit->lane);
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
  // Highlight the hovered road, prop, or junction floor (mutually exclusive);
  // repaint only when it actually changes so plain mouse-overs stay cheap.
  set_hovered_road(new_hover_road);
  set_hovered_object(new_hover_object);
  set_hovered_signal(new_hover_signal);
  set_hovered_junction(new_hover_junction);
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

void ViewportWidget::set_hovered_object(ObjectId object) {
  if (hover_locked_ || hovered_object_ == object) {
    return;
  }
  hovered_object_ = object;
  update();
}

void ViewportWidget::set_hovered_signal(SignalId signal) {
  if (hover_locked_ || hovered_signal_ == signal) {
    return;
  }
  hovered_signal_ = signal;
  update();
}

void ViewportWidget::set_hovered_junction(JunctionId junction) {
  if (hover_locked_ || hovered_junction_ == junction) {
    return;
  }
  hovered_junction_ = junction;
  update();
}

void ViewportWidget::leaveEvent(QEvent* event) {
  // Cursor left the viewport — drop every hover highlight.
  set_hovered_road({});
  set_hovered_object({});
  set_hovered_signal({});
  set_hovered_junction({});
  QOpenGLWidget::leaveEvent(event);
}

bool ViewportWidget::has_library_mime(const QDropEvent* event) {
  return event->mimeData()->hasFormat(QString::fromLatin1(kLibraryItemMimeType));
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* event) {
  if (!has_library_mime(event)) {
    return;
  }
  event->acceptProposedAction();
  emit_drag_preview_request(event);
}

void ViewportWidget::dragMoveEvent(QDragMoveEvent* event) {
  if (!has_library_mime(event)) {
    return;
  }
  event->acceptProposedAction();
  emit_drag_preview_request(event);
}

void ViewportWidget::dragLeaveEvent(QDragLeaveEvent* event) {
  clear_drop_preview();
  QOpenGLWidget::dragLeaveEvent(event);
}

void ViewportWidget::dropEvent(QDropEvent* event) {
  drop_preview_.reset();
  if (!has_library_mime(event)) {
    update();
    return;
  }
  event->acceptProposedAction();
  const QString key =
      QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kLibraryItemMimeType)));
  // Resolve the drop against the surface under the cursor (pick(), ground
  // fallback) — the same projection the drag preview used, so the element lands
  // where the ghost showed it (ghost==commit).
  const auto world = drop_world_point(event->position());
  const double world_x = world ? (*world)[0] : 0.0;
  const double world_y = world ? (*world)[1] : 0.0;
  update();
  emit library_item_dropped(key, world_x, world_y);
}

void ViewportWidget::emit_drag_preview_request(const QDropEvent* event) {
  const QString key =
      QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kLibraryItemMimeType)));
  const auto world = drop_world_point(event->position());
  emit library_item_drag_moved(key, world ? (*world)[0] : 0.0, world ? (*world)[1] : 0.0);
}

void ViewportWidget::set_drop_preview(double world_x, double world_y, bool valid) {
  drop_preview_ = DropPreview{.x = world_x, .y = world_y, .valid = valid};
  update();
}

void ViewportWidget::clear_drop_preview() {
  if (drop_preview_.has_value()) {
    drop_preview_.reset();
    update();
  }
}

void ViewportWidget::draw_drag_ghost(QPainter& painter) const {
  if (!drop_preview_.has_value()) {
    return;
  }
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  const auto screen = project_to_screen(camera_.matrices(aspect),
                                        drop_preview_->x,
                                        drop_preview_->y,
                                        0.0,
                                        static_cast<double>(width()),
                                        static_cast<double>(height()));
  if (!screen.has_value()) {
    return; // landing point behind the camera
  }
  const Theme& theme = theme::current();
  const QPointF center{(*screen)[0], (*screen)[1]};
  // A valid drop reads in the accent; a rejected one (no road for a prop) tints
  // to the warning color so the user sees it will not land there.
  const QColor color = drop_preview_->valid ? theme.accent : theme.warning;
  constexpr double kRadius = 9.0;
  painter.save();
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(color, 2, drop_preview_->valid ? Qt::SolidLine : Qt::DashLine));
  painter.drawEllipse(center, kRadius, kRadius);
  painter.setPen(QPen(color, 2));
  painter.drawLine(QPointF(center.x() - kRadius, center.y()),
                   QPointF(center.x() + kRadius, center.y()));
  painter.drawLine(QPointF(center.x(), center.y() - kRadius),
                   QPointF(center.x(), center.y() + kRadius));
  painter.restore();
}

// --- transform gizmo (A3, #177) --------------------------------------------

namespace {
constexpr double kGizmoYawDetent = std::numbers::pi / 12.0; // 15°
const QColor kAxisColorX{0xE0, 0x5A, 0x47};                 // red
const QColor kAxisColorY{0x4C, 0xB8, 0x5A};                 // green
const QColor kAxisColorZ{0x4A, 0x90, 0xE0};                 // blue
} // namespace

std::optional<ViewportWidget::GizmoTarget> ViewportWidget::gizmo_target() const {
  // The gizmo belongs to the Move tool, on a single road or prop.
  if (tools_.active_id() != ToolId::Move) {
    return std::nullopt;
  }
  const SelectionEntry sel = selection_.primary();
  if (sel.object.is_valid()) {
    const Object* object = document_.network().object(sel.object);
    if (object == nullptr) {
      return std::nullopt;
    }
    const Road* road = document_.network().road(object->road);
    if (road == nullptr || road->plan_view.empty()) {
      return std::nullopt;
    }
    const auto p = station_to_world(road->plan_view, object->s, object->t);
    return GizmoTarget{.object = sel.object, .pivot = {p[0], p[1], 0.0}};
  }
  if (sel.road.is_valid()) {
    const Road* road = document_.network().road(sel.road);
    if (road == nullptr || road->plan_view.empty()) {
      return std::nullopt;
    }
    const PathPoint pose = road->plan_view.evaluate(road->plan_view.length() / 2.0);
    return GizmoTarget{.road = sel.road, .pivot = {pose.x, pose.y, 0.0}};
  }
  return std::nullopt;
}

void ViewportWidget::draw_gizmo(QPainter& painter) const {
  const auto target = gizmo_target();
  if (!target.has_value()) {
    return;
  }
  const bool is_prop = target->object.is_valid();
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  const GizmoScreen gizmo = gizmo_screen(camera_.matrices(aspect),
                                         target->pivot,
                                         static_cast<double>(width()),
                                         static_cast<double>(height()));
  if (!gizmo.valid) {
    return;
  }
  const Theme& theme = theme::current();
  const QPointF origin(gizmo.origin[0], gizmo.origin[1]);
  const GizmoHandle active = gizmo_drag_.has_value() ? gizmo_drag_->handle : gizmo_hover_;

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(Qt::NoBrush);

  // Yaw ring (around Z).
  {
    const bool on = active == GizmoHandle::YawRing;
    painter.setPen(QPen(on ? theme.accent.lighter(140) : theme.accent, on ? 3.0 : 2.0));
    painter.drawEllipse(origin, gizmo.ring_radius, gizmo.ring_radius);
  }

  // Axis arrows (X red, Y green, Z blue) — Z only for roads (props have no
  // kernel z-move op yet).
  const auto draw_arrow = [&](GizmoHandle handle, std::array<double, 2> tip, QColor color) {
    if (std::hypot(tip[0] - gizmo.origin[0], tip[1] - gizmo.origin[1]) < 1e-6) {
      return; // arm points almost straight at the camera — hide it
    }
    const bool on = active == handle;
    painter.setPen(QPen(on ? color.lighter(150) : color, on ? 3.0 : 2.4));
    const QPointF tip_pt(tip[0], tip[1]);
    painter.drawLine(origin, tip_pt);
    // Arrowhead: a short filled triangle at the tip along the arm direction.
    const double dx = tip[0] - gizmo.origin[0];
    const double dy = tip[1] - gizmo.origin[1];
    const double len = std::hypot(dx, dy);
    const double ux = dx / len;
    const double uy = dy / len;
    constexpr double kHead = 9.0;
    constexpr double kWide = 4.0;
    const QPointF base(tip[0] - (ux * kHead), tip[1] - (uy * kHead));
    const QPolygonF head{{tip_pt,
                          QPointF(base.x() - (uy * kWide), base.y() + (ux * kWide)),
                          QPointF(base.x() + (uy * kWide), base.y() - (ux * kWide))}};
    painter.setBrush(on ? color.lighter(150) : color);
    painter.drawPolygon(head);
    painter.setBrush(Qt::NoBrush);
  };
  draw_arrow(GizmoHandle::AxisX, gizmo.x_tip, kAxisColorX);
  draw_arrow(GizmoHandle::AxisY, gizmo.y_tip, kAxisColorY);
  if (!is_prop) {
    draw_arrow(GizmoHandle::AxisZ, gizmo.z_tip, kAxisColorZ);
  }

  // Centre pad (free planar move).
  {
    const bool on = active == GizmoHandle::PlaneXY;
    const QColor color = on ? theme.accent : theme.text_secondary;
    painter.setPen(QPen(color, 1.5));
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), on ? 90 : 50));
    painter.drawRect(QRectF(origin.x() - gizmo.pad_half,
                            origin.y() - gizmo.pad_half,
                            gizmo.pad_half * 2.0,
                            gizmo.pad_half * 2.0));
  }
  painter.restore();
}

bool ViewportWidget::begin_gizmo_drag(const QPointF& pos) {
  const auto target = gizmo_target();
  if (!target.has_value()) {
    return false;
  }
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
  const GizmoScreen gizmo = gizmo_screen(camera_.matrices(aspect),
                                         target->pivot,
                                         static_cast<double>(width()),
                                         static_cast<double>(height()));
  GizmoHandle handle = gizmo_hit_test(gizmo, {pos.x(), pos.y()});
  // Props have no z-move: treat a Z-arm grab (they don't draw it) as a miss.
  if (handle == GizmoHandle::None || (handle == GizmoHandle::AxisZ && target->object.is_valid())) {
    return false;
  }
  const auto ground = ground_point_at(pos);
  const std::array<double, 2> press_world =
      ground ? std::array<double, 2>{(*ground)[0], (*ground)[1]}
             : std::array<double, 2>{target->pivot[0], target->pivot[1]};
  double base_hdg = 0.0;
  if (target->object.is_valid()) {
    if (const Object* object = document_.network().object(target->object)) {
      base_hdg = object->hdg;
    }
  }
  gizmo_drag_ = GizmoDrag{.handle = handle,
                          .road = target->road,
                          .object = target->object,
                          .pivot = target->pivot,
                          .press_world = press_world,
                          .press_px = pos.toPoint(),
                          .base_hdg = base_hdg,
                          .summary = QString()};
  update();
  return true;
}

void ViewportWidget::update_gizmo_drag(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
  if (!gizmo_drag_.has_value()) {
    return;
  }
  GizmoDrag& drag = *gizmo_drag_;
  const auto ground = ground_point_at(pos);
  const std::array<double, 2> cursor =
      ground ? std::array<double, 2>{(*ground)[0], (*ground)[1]} : drag.press_world;

  // Build the preview factory (const RoadNetwork& base -> command) for this
  // handle + entity; base is the pre-drag network (the session reverts to it
  // each frame), so all deltas are absolute from the press.
  std::function<std::unique_ptr<edit::Command>(const RoadNetwork&)> factory;
  const bool is_prop = drag.object.is_valid();

  if (drag.handle == GizmoHandle::YawRing) {
    const double detent = (modifiers & Qt::ShiftModifier) != 0 ? 0.0 : kGizmoYawDetent;
    const double angle =
        gizmo_yaw_angle({drag.pivot[0], drag.pivot[1]}, drag.press_world, cursor, detent);
    const double degrees = angle * 180.0 / std::numbers::pi;
    if (is_prop) {
      const ObjectId object = drag.object;
      const double base_hdg = drag.base_hdg;
      factory = [object, base_hdg, angle](const RoadNetwork& base) {
        const Object* o = base.object(object);
        return o != nullptr ? edit::move_object(base, object, o->s, o->t, base_hdg + angle)
                            : std::unique_ptr<edit::Command>{};
      };
      drag.summary = tr("Rotated prop by %1°").arg(degrees, 0, 'f', 0);
    } else {
      const RoadId road = drag.road;
      const std::array<double, 3> pivot = drag.pivot;
      factory = [road, angle, pivot](const RoadNetwork& base) {
        return edit::rotate_road(base, road, angle, pivot[0], pivot[1]);
      };
      const Road* r = document_.network().road(drag.road);
      drag.summary = tr("Rotated road %1 by %2°")
                         .arg(r != nullptr ? QString::fromStdString(r->odr_id) : QString())
                         .arg(degrees, 0, 'f', 0);
    }
  } else if (drag.handle == GizmoHandle::AxisZ) {
    // Vertical drag → world Z via the pivot's screen pixels-per-metre.
    const auto o =
        project_to_screen(camera_.matrices(static_cast<float>(
                              height() > 0 ? static_cast<double>(width()) / height() : 1.0)),
                          drag.pivot[0],
                          drag.pivot[1],
                          drag.pivot[2],
                          static_cast<double>(width()),
                          static_cast<double>(height()));
    const auto up =
        project_to_screen(camera_.matrices(static_cast<float>(
                              height() > 0 ? static_cast<double>(width()) / height() : 1.0)),
                          drag.pivot[0],
                          drag.pivot[1],
                          drag.pivot[2] + 1.0,
                          static_cast<double>(width()),
                          static_cast<double>(height()));
    double dz = 0.0;
    if (o.has_value() && up.has_value()) {
      const double ppm = (*o)[1] - (*up)[1]; // screen-y up = smaller, so +z → -y
      if (std::abs(ppm) > 1e-6) {
        dz = (static_cast<double>(drag.press_px.y()) - pos.y()) / ppm;
      }
    }
    const RoadId road = drag.road;
    factory = [road, dz](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
      const Road* r = base.road(road);
      if (r == nullptr) {
        return {};
      }
      std::vector<edit::ElevationPoint> points = edit::elevation_profile_points(*r);
      if (points.empty()) {
        points = {edit::ElevationPoint{.s = 0.0, .z = dz},
                  edit::ElevationPoint{.s = r->plan_view.length(), .z = dz}};
      } else {
        for (edit::ElevationPoint& point : points) {
          point.z += dz;
        }
      }
      return edit::set_elevation_profile(base, road, std::move(points));
    };
    drag.summary = tr("Raised road by %1 m").arg(dz, 0, 'f', 2);
  } else {
    // Planar translate (AxisX / AxisY / PlaneXY).
    const auto delta = gizmo_constrain_translation(drag.handle, drag.press_world, cursor);
    if (is_prop) {
      const ObjectId object = drag.object;
      const double nx = drag.pivot[0] + delta[0];
      const double ny = drag.pivot[1] + delta[1];
      factory = [object, nx, ny](const RoadNetwork& base) -> std::unique_ptr<edit::Command> {
        const Object* o = base.object(object);
        const Road* r = o != nullptr ? base.road(o->road) : nullptr;
        if (r == nullptr) {
          return {};
        }
        // Same road-relative guard as the prop move-drag: a gizmo dragged clear
        // of the road yields no command, so update_preview keeps the last good
        // frame rather than flinging the prop out to a huge t.
        const std::optional<StationCoord> station =
            station_within(r->plan_view, nx, ny, kObjectSnapThreshold);
        if (!station.has_value()) {
          return {};
        }
        return edit::move_object(base, object, station->s, station->t);
      };
      drag.summary = tr("Moved prop");
    } else {
      const RoadId road = drag.road;
      const double dx = delta[0];
      const double dy = delta[1];
      factory = [road, dx, dy](const RoadNetwork& base) {
        return edit::translate_roads(base, std::array<RoadId, 1>{road}, dx, dy);
      };
      drag.summary = tr("Moved road");
    }
  }

  if (!factory) {
    return;
  }
  const auto result = document_.preview_active()
                          ? document_.update_preview(factory)
                          : document_.begin_preview(factory(document_.network()));
  static_cast<void>(result);
}

void ViewportWidget::commit_gizmo_drag() {
  if (!gizmo_drag_.has_value()) {
    return;
  }
  const QString summary = gizmo_drag_->summary;
  const bool had_preview = document_.preview_active();
  document_.commit_preview();
  gizmo_drag_.reset();
  if (had_preview && !summary.isEmpty()) {
    show_toast(tr("%1 — Ctrl+Z to undo").arg(summary), ToastSeverity::Success);
  }
  update();
}

void ViewportWidget::cancel_gizmo_drag() {
  if (!gizmo_drag_.has_value()) {
    return;
  }
  document_.cancel_preview();
  gizmo_drag_.reset();
  update();
}

// Button map: navigation chords first (P1/GW-1 — see nav_controller.hpp and
// docs/user-guide/camera-navigation.md), then the M2 editing map
// (docs/design/m2/01_editing_framework.md §4): plain LMB drives the gizmo and
// the active tool (click-select, rubber band, node drag live in SelectTool).
// Chords, MMB, and the wheel never reach a tool.

void ViewportWidget::sync_pan_anchor(const QPointF& pos) {
  if (nav_.gesture() != NavGesture::Pan) {
    pan_anchor_world_.reset();
    return;
  }
  if (!pan_anchor_world_) {
    // Grab the ground point under the cursor; the anchored pan keeps it pinned.
    // Cap the ray so a near-horizon grab at low pitch falls back to view-plane.
    pan_anchor_world_ = ground_point_at(pos, 10.0 * camera_.distance());
  }
}

void ViewportWidget::apply_nav_move(const QPoint& delta, const QPointF& pos) {
  constexpr float kOrbitRadiansPerPixel = 0.008F;
  constexpr float kZoomStepsPerPixel = 0.02F;

  switch (nav_.gesture()) {
  case NavGesture::Orbit:
  case NavGesture::LegacyOrbit:
    camera_.orbit(static_cast<float>(-delta.x()) * kOrbitRadiansPerPixel,
                  static_cast<float>(delta.y()) * kOrbitRadiansPerPixel);
    break;
  case NavGesture::Pan: {
    // Ground-anchored pan: keep the grabbed world point under the cursor at 1:1
    // (CAD/maps feel, zero tuning constants). Degenerate near-horizon rays (no
    // anchor, or the current ray misses / is capped) fall back to a
    // correctly-scaled view-plane pan.
    const std::optional<std::array<double, 3>> current =
        ground_point_at(pos, 10.0 * camera_.distance());
    if (pan_anchor_world_ && current) {
      camera_.move_target(static_cast<float>((*pan_anchor_world_)[0] - (*current)[0]),
                          static_cast<float>((*pan_anchor_world_)[1] - (*current)[1]));
    } else {
      camera_.pan_pixels(static_cast<float>(delta.x()),
                         static_cast<float>(delta.y()),
                         static_cast<float>(height()));
    }
    break;
  }
  case NavGesture::ZoomDrag:
    // Drag up = zoom in (GW-1 step 3), matching the wheel's forward = closer.
    camera_.zoom(static_cast<float>(-delta.y()) * kZoomStepsPerPixel);
    break;
  case NavGesture::PivotVertical:
    camera_.elevate_target_pixels(static_cast<float>(delta.y()), static_cast<float>(height()));
    break;
  case NavGesture::None:
  case NavGesture::ContextPending:
    break;
  }
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->pos();
  // A live gizmo drag owns the mouse until its release — a chord must not
  // hijack it mid-edit (and, as before P1, cannot pan out from under it).
  if (!gizmo_drag_.has_value() &&
      nav_.press(event->button(), event->buttons(), event->modifiers(), event->pos())) {
    sync_pan_anchor(event->position());
    event->accept();
    return;
  }
  // A gizmo handle grab takes LMB before the tool sees it.
  if (event->button() == Qt::LeftButton && begin_gizmo_drag(event->position())) {
    event->accept();
    return;
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

  // Drive an active gizmo drag before anything else.
  if (gizmo_drag_.has_value()) {
    update_gizmo_drag(event->position(), event->modifiers());
    update();
    event->accept();
    return;
  }
  // A live navigation chord owns the mouse: the gizmo, the tool, and hover all
  // stay quiet until it ends. A right-press that hasn't passed the slop yet is
  // not live, so it falls through and leaves hover exactly as it was.
  if (nav_.move(
          event->pos(),
          event->buttons(),
          static_cast<int>(std::lround(NavController::kContextDragSlop * devicePixelRatioF())))) {
    apply_nav_move(delta, event->position());
    update();
    event->accept();
    return;
  }
  // Idle hover: highlight the gizmo handle under the cursor (no buttons held).
  if (event->buttons() == Qt::NoButton) {
    GizmoHandle hover = GizmoHandle::None;
    if (const auto target = gizmo_target()) {
      const float aspect =
          height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
      hover = gizmo_hit_test(gizmo_screen(camera_.matrices(aspect),
                                          target->pivot,
                                          static_cast<double>(width()),
                                          static_cast<double>(height())),
                             {event->position().x(), event->position().y()});
      if (target->object.is_valid() && hover == GizmoHandle::AxisZ) {
        hover = GizmoHandle::None; // props don't expose Z
      }
    }
    if (hover != gizmo_hover_) {
      gizmo_hover_ = hover;
      update();
    }
    // A handle under the cursor swallows hover so the tool's readout stays calm.
    if (hover != GizmoHandle::None) {
      setCursor(hover == GizmoHandle::YawRing ? Qt::PointingHandCursor : Qt::SizeAllCursor);
      event->accept();
      return;
    }
  }

  if (Tool* tool = tools_.active(); tool != nullptr && tool->mouse_move(make_tool_event(event))) {
    update();
    event->accept();
    return;
  }

  // A right-press that hasn't passed the slop is a pending click, not a hover:
  // leave the highlight where it was until the button resolves (as before P1).
  if (nav_.gesture() != NavGesture::ContextPending) {
    update_hover(event->position());
  }
  event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && gizmo_drag_.has_value()) {
    commit_gizmo_drag();
    event->accept();
    return;
  }
  bool context_click = false;
  if (nav_.release(event->button(), event->buttons(), &context_click)) {
    // The chord may have unwound onto another gesture (⌥+LMB+RMB pan →
    // ⌥+LMB orbit), so the anchor is re-synced rather than simply dropped.
    sync_pan_anchor(event->position());
    if (context_click) {
      emit context_menu_requested(build_menu_context(event->position()),
                                  event->globalPosition().toPoint());
    }
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
  const Ray ray = ray_through(pos);
  MenuContext context =
      menu_context_for_pick(document_.network(), pick(document_.mesh(), road_aabbs_, ray));

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
  // Esc cancels an in-flight gizmo drag before the tool sees the key.
  if (event->key() == Qt::Key_Escape && gizmo_drag_.has_value()) {
    cancel_gizmo_drag();
    event->accept();
    return;
  }
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
  // Station of the cursor along the picked road's reference line — only when a
  // road was hit (a lane/road body); empty-space clicks carry no station.
  if (tool_event.pick.has_value()) {
    if (const Road* road = document_.network().road(tool_event.pick->road)) {
      tool_event.station = find_station(road->plan_view, tool_event.world_x, tool_event.world_y);
    }
  }
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
    // Zoom toward the cursor. In perspective the eye already travels along the
    // cursor's ray, so this only changes ortho — where zoom_about shifts the
    // pivot to keep the anchored point under the cursor.
    const float w = std::max(static_cast<float>(width()), 1.0F);
    const float h = std::max(static_cast<float>(height()), 1.0F);
    const std::array<float, 2> anchor_ndc{
        (2.0F * static_cast<float>(event->position().x()) / w) - 1.0F,
        1.0F - (2.0F * static_cast<float>(event->position().y()) / h), // Qt y is down
    };
    camera_.zoom_about(steps, anchor_ndc, w / h);
    update();
  }
  event->accept();
}

void ViewportWidget::apply_render_mode() {
  renderer_->set_environment(textured_rendering_ ? textured_lighting() : sober_lighting());

  // In textured mode the reference grid is scenery-competing clutter, so fade
  // it heavily (the daytime surfaces carry the scene); sober mode keeps the
  // full-strength grid it has always had.
  BackdropColors backdrop = theme::current().backdrop();
  if (textured_rendering_) {
    backdrop.grid_major[3] *= 0.35F;
    backdrop.grid_minor[3] *= 0.35F;
  }
  renderer_->set_backdrop(backdrop);

  // Procedural grass ground in textured mode only, sitting just under the
  // network floor so coplanar road surfaces draw over it (Sober keeps the grid).
  renderer_->set_ground(textured_rendering_, ground_base_z(scene_bounds_));
}

void ViewportWidget::set_textured_rendering(bool textured) {
  if (textured == textured_rendering_) {
    return;
  }
  textured_rendering_ = textured;
  apply_render_mode();
  update();
}

void ViewportWidget::reset_camera() {
  camera_ = OrbitCamera{};
  if (scene_bounds_.valid()) {
    camera_.frame(scene_bounds_.center(), scene_bounds_.framing_radius());
  }
  update();
}

void ViewportWidget::frame_selection() {
  // With a selection: frame exactly what is selected, per kind (framing.hpp).
  if (!selection_.empty()) {
    const SceneBounds bounds = selection_bounds(document_.mesh(), selection_.entries());
    if (bounds.valid()) {
      camera_.frame(bounds.center(), bounds.framing_radius());
      update();
    }
    return; // a selection that resolves to nothing leaves the view alone
  }
  // No selection: frame all content, keeping the viewing angle (GW-1 step 8) —
  // frame() only moves the pivot and distance, never yaw/pitch.
  if (scene_bounds_.valid()) {
    camera_.frame(scene_bounds_.center(), scene_bounds_.framing_radius());
    update();
    return;
  }
  // Empty scene: back to the origin pivot (GW-1 step 9), angle preserved.
  camera_.frame(OrbitCamera{}.target(), 0.0F);
  update();
}

void ViewportWidget::frame_cursor() {
  // Frame-on-cursor is a PIVOT move: the camera keeps its angle and its zoom
  // distance, so the view swings to a new point of interest without dollying
  // (GW-1 step 10).
  const QPointF pos = mapFromGlobal(QCursor::pos());
  if (!rect().contains(pos.toPoint())) {
    return; // cursor outside the viewport — nothing to frame on
  }
  if (const auto point = drop_world_point(pos)) {
    camera_.look_at({static_cast<float>((*point)[0]),
                     static_cast<float>((*point)[1]),
                     static_cast<float>((*point)[2])});
    update();
  }
}

void ViewportWidget::set_projection(ProjectionMode mode) {
  if (camera_.projection() == mode) {
    return;
  }
  camera_.set_projection(mode);
  update();
}

void ViewportWidget::look_from(CardinalView view) {
  // Cardinals snap YAW only (and pitch for top-down): the pivot and zoom stay
  // put, so pressing one re-angles the view without losing your place.
  constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0F;
  switch (view) {
  case CardinalView::North: // looking south from the north
    camera_.set_view(kHalfPi, camera_.pitch());
    break;
  case CardinalView::South:
    camera_.set_view(-kHalfPi, camera_.pitch());
    break;
  case CardinalView::West:
    camera_.set_view(std::numbers::pi_v<float>, camera_.pitch());
    break;
  case CardinalView::East:
    camera_.set_view(0.0F, camera_.pitch());
    break;
  case CardinalView::Top:
    // North-up plan view: yaw −π/2 puts +y (north) at the top of the screen.
    camera_.set_view(-kHalfPi, OrbitCamera::kTopDownPitch);
    break;
  }
  update();
}

} // namespace roadmaker::editor
