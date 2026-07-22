// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "tools/marking_point_tool.hpp"

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// Cursor travel [m] past which a press on a stencil becomes a drag rather than
/// a click (matches SelectTool's click tolerance).
constexpr double kDragTolerance = 0.5;

/// Is `object` a stencil this tool may drag? Only objects it authored carry
/// rm:stencil data, so a press on a tree or a crosswalk never starts a stencil
/// move.
bool is_stencil(const RoadNetwork& network, ObjectId object) {
  const Object* obj = network.object(object);
  return obj != nullptr && obj->stencil.has_value();
}

/// The ghost arrow polygon for `pose` and `item`, appended to `geometry` as a
/// closed loop of world segments plus an anchor handle at the glyph origin. The
/// glyph's local (u = along travel, v = leftward) frame maps to world through the
/// lane pose, so the ghost sits exactly where the placed object will draw.
void append_glyph(PreviewGeometry& geometry,
                  const RoadNetwork& network,
                  const StencilPose& pose,
                  const LibraryItem& item) {
  const Road* road = network.road(pose.road);
  if (road == nullptr || road->plan_view.empty()) {
    return;
  }
  const double width = std::max(pose.lane_width_m, 0.1) * item.stencil_width_frac;
  const std::vector<roadmaker::OutlineCorner> outline =
      edit::arrow_glyph_outline(item.stencil_subtype.toStdString(), item.stencil_length, width);
  if (outline.size() < 3) {
    return;
  }
  const std::array<double, 2> origin = station_to_world(road->plan_view, pose.s, pose.t);
  const double cos_h = std::cos(pose.hdg);
  const double sin_h = std::sin(pose.hdg);
  const auto to_world = [&](const roadmaker::OutlineCorner& c) {
    // local u along travel (cos, sin), v leftward (-sin, cos).
    return std::array<double, 2>{origin[0] + (c.a * cos_h) - (c.b * sin_h),
                                 origin[1] + (c.a * sin_h) + (c.b * cos_h)};
  };
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const std::array<double, 2> a = to_world(outline[i]);
    const std::array<double, 2> b = to_world(outline[(i + 1) % outline.size()]);
    geometry.line_positions.insert(geometry.line_positions.end(),
                                   {a[0], a[1], 0.0, b[0], b[1], 0.0});
  }
  geometry.add_handle(origin[0], origin[1], 0.0, HandleKind::Node, HandleState::Hovered);
}

} // namespace

MarkingPointTool::MarkingPointTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void MarkingPointTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem MarkingPointTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void MarkingPointTool::deactivate() {
  if (drag_.has_value()) {
    document_.cancel_preview();
  }
  reset();
}

void MarkingPointTool::reset() {
  press_.reset();
  drag_.reset();
  hover_.reset();
  emit preview_changed();
}

bool MarkingPointTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // A press on an existing stencil arms a drag; anywhere else arms a click-place
  // (resolved on release). LMB belongs to the tool either way so it never leaks
  // to camera navigation.
  const bool on_stencil = event.pick.has_value() && event.pick->object.is_valid() &&
                          is_stencil(document_.network(), event.pick->object);
  press_ =
      PressState{.world_x = event.world_x,
                 .world_y = event.world_y,
                 .object = on_stencil ? std::optional<ObjectId>(event.pick->object) : std::nullopt,
                 .object_road = on_stencil ? event.pick->road : RoadId{}};
  return true;
}

bool MarkingPointTool::mouse_move(const ToolEvent& event) {
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
  // Plain hover: preview the ghost glyph at the lane under the cursor. Return
  // false so the viewport's hover readout still runs.
  hover_ = stencil_pose_for_point(document_.network(), event.world_x, event.world_y);
  emit preview_changed();
  return false;
}

bool MarkingPointTool::mouse_release(const ToolEvent& event) {
  if (drag_.has_value()) {
    // A drag that never crossed the tolerance / a fit commits nothing (no-op).
    const RoadId road = drag_->road;
    const ObjectId object = drag_->object;
    document_.commit_preview();
    drag_.reset();
    press_.reset();
    select_object(road, object);
    emit status_message(tr("Stencil moved — Ctrl+Z to undo"));
    emit preview_changed();
    return true;
  }
  if (press_.has_value()) {
    const std::optional<ObjectId> object = press_->object;
    const RoadId object_road = press_->object_road;
    press_.reset();
    if (object.has_value()) {
      // A click on a stencil (no drag) selects it.
      select_object(object_road, *object);
      emit preview_changed();
      return true;
    }
    place_stencil(event.world_x, event.world_y);
    return true;
  }
  return false;
}

bool MarkingPointTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
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

void MarkingPointTool::place_stencil(double world_x, double world_y) {
  const LibraryItem item = current_item();
  if (item.kind != LibraryItem::Kind::Stencil) {
    emit toast_requested(tr("Select an arrow stencil in the Library to place one"),
                         ToastSeverity::Warning);
    return;
  }
  std::optional<std::pair<RoadId, Object>> placed =
      stencil_for_point(document_.network(), world_x, world_y, item, materials_);
  if (!placed.has_value()) {
    emit status_message(tr("Aim at a lane to place the arrow stencil"));
    return;
  }
  const RoadId road = placed->first;
  const std::string odr = placed->second.odr_id;
  // ONE undo entry: a single add_object is already one undo step (no macro).
  if (!document_
           .push_command(edit::add_object(document_.network(), road, std::move(placed->second)))
           .has_value()) {
    emit status_message(tr("Couldn't place the arrow stencil here"));
    return;
  }
  // Select the placed stencil (add_object mints the ObjectId only on apply, so
  // look it up by its odr_id on the owning road).
  document_.network().for_each_object([&](ObjectId id, const Object& object) {
    if (object.road == road && object.odr_id == odr) {
      selection_.select({.road = road, .object = id}, SelectMode::Replace);
    }
  });
  hover_.reset();
  emit status_message(tr("Placed arrow stencil — Ctrl+Z to undo"));
  emit preview_changed();
}

void MarkingPointTool::begin_drag(ObjectId object, RoadId road) {
  drag_ = DragState{.object = object, .road = road};
  // Auto-select the grabbed stencil so the properties panel tracks it.
  select_object(road, object);
  press_.reset();
  hover_.reset();
  emit cursor_changed(Qt::SizeAllCursor);
  emit status_message(tr("Moving stencil — release to place, Esc cancels"));
}

void MarkingPointTool::update_drag(double world_x, double world_y) {
  if (!drag_.has_value()) {
    return;
  }
  // Re-project onto the OWNING road (move_object re-locates on the same road),
  // clamp t to the lane band, and follow the lane's travel heading. A cursor
  // dragged clear of the road holds the stencil at its last good pose.
  const std::optional<StencilPose> pose =
      stencil_pose_on_road(document_.network(), drag_->road, world_x, world_y);
  if (!pose.has_value()) {
    if (!drag_->off_road) {
      drag_->off_road = true;
      emit status_message(tr("Keep the stencil on its road — Esc cancels"));
    }
    return;
  }
  if (drag_->off_road) {
    drag_->off_road = false;
    emit status_message(tr("Moving stencil — release to place, Esc cancels"));
  }
  const ObjectId object = drag_->object;
  const double s = pose->s;
  const double t = pose->t;
  const double hdg = pose->hdg;
  const Expected<void> moved =
      document_.preview_active()
          ? document_.update_preview([object, s, t, hdg](const RoadNetwork& base) {
              return edit::move_object(base, object, s, t, hdg);
            })
          : document_.begin_preview(edit::move_object(document_.network(), object, s, t, hdg));
  static_cast<void>(moved);
}

void MarkingPointTool::select_object(RoadId road, ObjectId object) {
  if (document_.network().object(object) == nullptr) {
    return;
  }
  selection_.select({.road = road, .object = object}, SelectMode::Replace);
}

PreviewGeometry MarkingPointTool::preview() const {
  PreviewGeometry geometry;
  // While dragging, the live preview session already moved the real object, so
  // no ghost is drawn (it would double the glyph). While hovering, the ghost
  // marks where a click lands.
  if (!drag_.has_value() && hover_.has_value()) {
    const LibraryItem item = current_item();
    if (item.kind == LibraryItem::Kind::Stencil) {
      append_glyph(geometry, document_.network(), *hover_, item);
    }
  }
  return geometry;
}

QString MarkingPointTool::instruction() const {
  return tr("Click a lane to place the selected arrow stencil");
}

} // namespace roadmaker::editor
