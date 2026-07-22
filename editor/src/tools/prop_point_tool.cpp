#include "tools/prop_point_tool.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// Cursor travel [m] past which a press on a prop becomes a drag rather than a
/// click (matches SelectTool's click tolerance).
constexpr double kDragTolerance = 0.5;

/// Segments in the ghost radius circle — smooth enough at typical zoom without
/// flooding the overlay line buffer.
constexpr int kGhostSegments = 24;

/// Is `object` a prop this tool may drag? Props are the instanced classes —
/// Tree/Vegetation plus Pole (streetlights) and Building; a press on a stencil,
/// crosswalk, or signal never starts a prop move.
bool is_prop(const RoadNetwork& network, ObjectId object) {
  const Object* obj = network.object(object);
  return obj != nullptr && (obj->type == ObjectType::Tree || obj->type == ObjectType::Vegetation ||
                            obj->type == ObjectType::Pole || obj->type == ObjectType::Building);
}

/// A closed ring of `radius` [m] around world (cx, cy), appended to `geometry` as
/// line segments plus an anchor handle at the centre — the prop's footprint ghost.
void append_ghost(PreviewGeometry& geometry, double cx, double cy, double radius) {
  const double r = std::max(radius, 0.25);
  std::array<double, 2> prev{cx + r, cy};
  for (int i = 1; i <= kGhostSegments; ++i) {
    const double a = (2.0 * std::numbers::pi * i) / kGhostSegments;
    const std::array<double, 2> cur{cx + (r * std::cos(a)), cy + (r * std::sin(a))};
    geometry.line_positions.insert(geometry.line_positions.end(),
                                   {prev[0], prev[1], 0.0, cur[0], cur[1], 0.0});
    prev = cur;
  }
  geometry.add_handle(cx, cy, 0.0, HandleKind::Node, HandleState::Hovered);
}

} // namespace

PropPointTool::PropPointTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void PropPointTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem PropPointTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void PropPointTool::deactivate() {
  if (drag_.has_value()) {
    document_.cancel_preview();
  }
  reset();
}

void PropPointTool::reset() {
  press_.reset();
  drag_.reset();
  hover_.reset();
  emit preview_changed();
}

bool PropPointTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // A press on an existing prop arms a drag; anywhere else arms a click-place
  // (resolved on release). LMB belongs to the tool either way so it never leaks
  // to camera navigation.
  const bool on_prop = event.pick.has_value() && event.pick->object.is_valid() &&
                       is_prop(document_.network(), event.pick->object);
  press_ =
      PressState{.world_x = event.world_x,
                 .world_y = event.world_y,
                 .object = on_prop ? std::optional<ObjectId>(event.pick->object) : std::nullopt,
                 .object_road = on_prop ? event.pick->road : RoadId{}};
  return true;
}

bool PropPointTool::mouse_move(const ToolEvent& event) {
  if (drag_.has_value()) {
    update_drag(event.world_x, event.world_y);
    emit preview_changed();
    return true;
  }
  if (press_.has_value()) {
    const bool beyond = std::abs(event.world_x - press_->world_x) > kDragTolerance ||
                        std::abs(event.world_y - press_->world_y) > kDragTolerance;
    if (press_->object.has_value() && beyond) {
      begin_drag(*press_->object, press_->object_road);
      if (drag_.has_value()) {
        update_drag(event.world_x, event.world_y);
        emit preview_changed();
      }
    }
    return true;
  }
  // Plain hover: preview the ghost footprint at the road nearest the cursor.
  // Return false so the viewport's hover readout still runs.
  hover_ =
      nearest_road_station(document_.network(), event.world_x, event.world_y, kObjectSnapThreshold);
  emit preview_changed();
  return false;
}

bool PropPointTool::mouse_release(const ToolEvent& event) {
  if (drag_.has_value()) {
    // A drag that never crossed the tolerance / a fit commits nothing (no-op).
    const RoadId road = drag_->road;
    const ObjectId object = drag_->object;
    document_.commit_preview();
    drag_.reset();
    press_.reset();
    select_object(road, object);
    emit status_message(tr("Prop moved — Ctrl+Z to undo"));
    emit preview_changed();
    return true;
  }
  if (press_.has_value()) {
    const std::optional<ObjectId> object = press_->object;
    const RoadId object_road = press_->object_road;
    press_.reset();
    if (object.has_value()) {
      // A click on a prop (no drag) selects it.
      select_object(object_road, *object);
      emit preview_changed();
      return true;
    }
    place_prop(event.world_x, event.world_y);
    return true;
  }
  return false;
}

bool PropPointTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape) {
    return false;
  }
  if (drag_.has_value()) {
    document_.cancel_preview();
    drag_.reset();
    press_.reset();
    emit cursor_changed(Qt::ArrowCursor);
    emit status_message(tr("Move cancelled"));
    emit preview_changed();
    return true;
  }
  if (press_.has_value()) {
    press_.reset();
    return true;
  }
  return false;
}

void PropPointTool::place_prop(double world_x, double world_y) {
  LibraryItem item = current_item();
  if (!is_prop_asset(item)) {
    emit toast_requested(tr("Select a tree or shrub in the Library to place one"),
                         ToastSeverity::Warning);
    return;
  }
  // A prop set carries no single model — draw one concrete prop from it (the
  // curve/span/polygon tools resolve per instance the same way). make_prop_object
  // requires a pre-resolved Tree, so this must happen before it (#367).
  if (item.kind == LibraryItem::Kind::PropSet) {
    std::mt19937 rng(place_seed_++);
    item = resolve_prop_asset(item, rng);
  }
  const std::optional<RoadStation> placement =
      nearest_road_station(document_.network(), world_x, world_y, kObjectSnapThreshold);
  if (!placement.has_value()) {
    emit status_message(tr("Aim at or beside a road to place the prop"));
    return;
  }
  Object prop =
      make_prop_object(item, next_object_odr_id(document_.network()), placement->s, placement->t);
  const RoadId road = placement->road;
  const std::string odr = prop.odr_id;
  // ONE undo entry: a single add_object is already one undo step (no macro).
  if (!document_.push_command(edit::add_object(document_.network(), road, std::move(prop)))
           .has_value()) {
    emit status_message(tr("Couldn't place the prop here"));
    return;
  }
  // Select the placed prop (add_object mints the ObjectId only on apply, so look
  // it up by its odr_id on the owning road).
  document_.network().for_each_object([&](ObjectId id, const Object& object) {
    if (object.road == road && object.odr_id == odr) {
      selection_.select({.road = road, .object = id}, SelectMode::Replace);
    }
  });
  hover_.reset();
  emit status_message(tr("Placed prop — Ctrl+Z to undo"));
  emit preview_changed();
}

void PropPointTool::begin_drag(ObjectId object, RoadId road) {
  drag_ = DragState{.object = object, .road = road};
  // Auto-select the grabbed prop so the properties panel tracks it.
  select_object(road, object);
  press_.reset();
  hover_.reset();
  emit cursor_changed(Qt::SizeAllCursor);
  emit status_message(tr("Moving prop — release to place, Esc cancels"));
}

void PropPointTool::update_drag(double world_x, double world_y) {
  if (!drag_.has_value()) {
    return;
  }
  // Re-project onto the OWNING road (move_object re-locates on the same road) and
  // clamp t to that road's reach. A cursor dragged clear of the road holds the
  // prop at its last good pose. Props are radially symmetric, so heading is left
  // unchanged (hdg stays 0, matching the Library tree drop).
  const Road* road = document_.network().road(drag_->road);
  if (road == nullptr || road->plan_view.empty()) {
    return;
  }
  const std::optional<StationCoord> station =
      station_within(road->plan_view, world_x, world_y, kObjectSnapThreshold);
  if (!station.has_value()) {
    if (!drag_->off_road) {
      drag_->off_road = true;
      emit status_message(tr("Keep the prop near its road — Esc cancels"));
    }
    return;
  }
  if (drag_->off_road) {
    drag_->off_road = false;
    emit status_message(tr("Moving prop — release to place, Esc cancels"));
  }
  const ObjectId object = drag_->object;
  const double s = std::clamp(station->s, 0.0, road->plan_view.length());
  const double t = station->t;
  const Expected<void> moved =
      document_.preview_active()
          ? document_.update_preview([object, s, t](const RoadNetwork& base) {
              return edit::move_object(base, object, s, t);
            })
          : document_.begin_preview(edit::move_object(document_.network(), object, s, t));
  static_cast<void>(moved);
}

void PropPointTool::select_object(RoadId road, ObjectId object) {
  if (document_.network().object(object) == nullptr) {
    return;
  }
  selection_.select({.road = road, .object = object}, SelectMode::Replace);
}

PreviewGeometry PropPointTool::preview() const {
  PreviewGeometry geometry;
  // While dragging, the live preview session already moved the real object, so no
  // ghost is drawn (it would double the footprint). While hovering, the ghost
  // marks where a click lands.
  if (!drag_.has_value() && hover_.has_value()) {
    const LibraryItem item = current_item();
    if (is_prop_asset(item)) {
      const Road* road = document_.network().road(hover_->road);
      if (road != nullptr && !road->plan_view.empty()) {
        const std::array<double, 2> origin =
            station_to_world(road->plan_view, hover_->s, hover_->t);
        double radius = 1.0;
        if (const props::PropModel* model = props::model(item.model.toStdString())) {
          radius = model->radius;
        }
        append_ghost(geometry, origin[0], origin[1], radius);
      }
    }
  }
  return geometry;
}

QString PropPointTool::instruction() const {
  return tr("Click on or beside a road to place the selected prop; drag a prop to move it");
}

} // namespace roadmaker::editor
