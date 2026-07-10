#include "viewport/viewport_widget.hpp"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "render/gl_functions.hpp"
#include "render/gl_renderer.hpp"
#include "render/scene_builder.hpp"

namespace roadmaker::editor {

namespace {

/// Captureless lambda decays to gl::ProcResolver; keeps render/ Qt-free.
void* qt_gl_resolver(const char* name) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  return context == nullptr ? nullptr : reinterpret_cast<void*>(context->getProcAddress(name));
}

constexpr int kClickDragThreshold = 3; // manhattan px

} // namespace

ViewportWidget::ViewportWidget(Document& document, SelectionModel& selection, QWidget* parent)
    : QOpenGLWidget(parent), document_(document), selection_(selection),
      renderer_(std::make_unique<GLRenderer>()) {
  setMouseTracking(true); // hover s/t without a pressed button
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 240);

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
  grid_ = renderer_->upload(make_grid());
  scene_dirty_ = true;
}

void ViewportWidget::rebuild_scene() {
  renderer_->clear_meshes();
  items_.clear();
  pending_roads_.clear();
  grid_ = renderer_->upload(make_grid());

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

  std::vector<DrawItem> draw_items;
  draw_items.reserve(items_.size() + 1);
  if (grid_.valid()) {
    draw_items.push_back(DrawItem{.mesh = grid_});
  }
  for (const UploadedItem& item : items_) {
    draw_items.push_back(DrawItem{.mesh = item.handle, .highlighted = is_highlighted(item)});
  }

  // Framebuffer pixels, not widget units (HiDPI rule).
  const auto fb_width = static_cast<int>(std::lround(width() * devicePixelRatioF()));
  const auto fb_height = static_cast<int>(std::lround(height() * devicePixelRatioF()));
  const float aspect =
      fb_height > 0 ? static_cast<float>(fb_width) / static_cast<float>(fb_height) : 1.0F;
  renderer_->render(draw_items, camera_.matrices(aspect), fb_width, fb_height);
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

void ViewportWidget::update_hover(const QPointF& pos) {
  HoverInfo info;
  const Ray ray = ray_through(pos);

  // Cursor world x/y: ray ∩ ground plane (z = 0).
  if (std::abs(ray.direction[2]) > 1e-12) {
    const double t = -ray.origin[2] / ray.direction[2];
    if (t >= 0.0) {
      info.valid = true;
      info.world_x = ray.origin[0] + (ray.direction[0] * t);
      info.world_y = ray.origin[1] + (ray.direction[1] * t);
    }
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

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
  press_pos_ = event->pos();
  last_mouse_pos_ = event->pos();
  event->accept();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
  const QPoint delta = event->pos() - last_mouse_pos_;
  last_mouse_pos_ = event->pos();

  if ((event->buttons() & Qt::LeftButton) != 0) {
    camera_.orbit(static_cast<float>(-delta.x()) * 0.008F, static_cast<float>(delta.y()) * 0.008F);
    update();
  } else if ((event->buttons() & (Qt::RightButton | Qt::MiddleButton)) != 0) {
    camera_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
    update();
  } else {
    update_hover(event->position());
  }
  event->accept();
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
  // A left click (press+release without a drag) picks; a miss clears.
  // Ctrl (Cmd on macOS) toggles the hit in and out of the selection.
  if (event->button() == Qt::LeftButton &&
      (event->pos() - press_pos_).manhattanLength() < kClickDragThreshold) {
    const bool toggle = (event->modifiers() & Qt::ControlModifier) != 0;
    if (const auto hit = pick(document_.mesh(), road_aabbs_, ray_through(event->position()))) {
      selection_.select({.road = hit->road, .lane = hit->lane},
                        toggle ? SelectMode::Toggle : SelectMode::Replace);
    } else if (!toggle) {
      selection_.clear();
    }
  }
  event->accept();
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
