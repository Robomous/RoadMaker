#include "tools/marking_curve_tool.hpp"

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/tol.hpp"

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/marking_curve_placement.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// A marking curve anchors to a road whose reference line passes within this
/// lateral distance [m] of the first click — the same reach the stencil/tree
/// drops use, so aiming anywhere across the carriageway grabs the road.
constexpr double kAnchorSnapThreshold = kObjectSnapThreshold;

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

} // namespace

MarkingCurveTool::MarkingCurveTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void MarkingCurveTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem MarkingCurveTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void MarkingCurveTool::deactivate() {
  reset_session();
}

bool MarkingCurveTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  place_point(event.world_x, event.world_y);
  return true;
}

bool MarkingCurveTool::mouse_move(const ToolEvent& event) {
  cursor_ = Waypoint{.x = event.world_x, .y = event.world_y};
  emit preview_changed();
  return false; // hovering never consumes: camera nav and hover readout stay live
}

bool MarkingCurveTool::mouse_double_click(const ToolEvent& event) {
  static_cast<void>(event); // the pair's first press already placed the point
  commit();
  return true;
}

bool MarkingCurveTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    commit();
    return true;
  }
  if (key == Qt::Key_Backspace) {
    if (points_.empty()) {
      return false;
    }
    points_.pop_back();
    if (points_.empty()) {
      anchor_.reset(); // the anchor was set by the first point; drop it with it
    }
    emit status_message(tr("Removed the last point"));
    emit preview_changed();
    return true;
  }
  if (key == Qt::Key_Escape) {
    if (points_.empty()) {
      return false;
    }
    reset_session();
    emit status_message(tr("Marking curve cancelled"));
    return true;
  }
  return false;
}

void MarkingCurveTool::place_point(double world_x, double world_y) {
  if (points_.empty()) {
    // The first point anchors the curve. Refuse if the asset is incompatible or
    // the click is off any road — nothing is placed until an anchor is found.
    if (!is_marking_curve_asset(current_item())) {
      emit toast_requested(tr("Select a crosswalk or marking asset in the Library first"),
                           ToastSeverity::Warning);
      return;
    }
    const std::optional<RoadId> anchor =
        anchor_road_at(document_.network(), world_x, world_y, kAnchorSnapThreshold);
    if (!anchor.has_value()) {
      emit status_message(tr("Click on a road to start the marking curve"));
      return;
    }
    anchor_ = anchor;
  }
  points_.push_back(Waypoint{.x = world_x, .y = world_y});
  emit status_message(tr("%n point(s) — Enter or double-click authors the marking",
                         nullptr,
                         static_cast<int>(points_.size())));
  emit preview_changed();
}

void MarkingCurveTool::commit() {
  if (points_.size() < 2 || !anchor_.has_value()) {
    emit status_message(tr("A marking curve needs at least two points"));
    return;
  }
  const edit::MarkingCurveParams params =
      marking_curve_params_from_item(current_item(), materials_);
  const Expected<MarkingCurveResult> result =
      build_marking_curve(document_.network(), *anchor_, points_, params);
  if (!result.has_value()) {
    // Session kept: the points are the user's work; fix and retry.
    emit status_message(
        tr("Cannot author marking curve: %1").arg(QString::fromStdString(result.error().message)));
    return;
  }
  const RoadId road = result->road;
  const std::string odr = result->object.odr_id;
  Object object = result->object;
  if (!document_.push_command(edit::add_object(document_.network(), road, std::move(object)))
           .has_value()) {
    emit status_message(tr("Couldn't author the marking curve here"));
    return;
  }
  // Select the placed curve (add_object mints the ObjectId only on apply).
  document_.network().for_each_object([&](ObjectId id, const Object& placed) {
    if (placed.road == road && placed.odr_id == odr) {
      selection_.select({.road = road, .object = id}, SelectMode::Replace);
    }
  });
  reset_session();
  emit status_message(tr("Marking curve authored — Ctrl+Z to undo"));
}

void MarkingCurveTool::reset_session() {
  points_.clear();
  anchor_.reset();
  cursor_.reset();
  emit preview_changed();
}

PreviewGeometry MarkingCurveTool::preview() const {
  PreviewGeometry geometry;

  for (const Waypoint& point : points_) {
    geometry.add_handle(point.x, point.y);
  }

  // Ghost polyline through the placed points and on to the cursor.
  for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
    append_segment(geometry, points_[i].x, points_[i].y, points_[i + 1].x, points_[i + 1].y);
  }
  if (!points_.empty() && cursor_.has_value()) {
    append_segment(geometry, points_.back().x, points_.back().y, cursor_->x, cursor_->y);
  }

  // The CLAMPED snapped centreline the commit will author (ghost==commit): the
  // placed points plus the cursor as a provisional last point, projected onto the
  // anchor road. A failed fit silently leaves just the raw ghost polyline.
  if (anchor_.has_value() && !points_.empty()) {
    std::vector<Waypoint> candidate = points_;
    if (cursor_.has_value() &&
        std::hypot(cursor_->x - points_.back().x, cursor_->y - points_.back().y) >= tol::kLength) {
      candidate.push_back(*cursor_);
    }
    if (candidate.size() >= 2) {
      const std::vector<std::array<double, 2>> snapped =
          marking_curve_preview_polyline(document_.network(), *anchor_, candidate);
      for (std::size_t i = 0; i + 1 < snapped.size(); ++i) {
        append_segment(
            geometry, snapped[i][0], snapped[i][1], snapped[i + 1][0], snapped[i + 1][1]);
      }
    }
  }

  return geometry;
}

QString MarkingCurveTool::instruction() const {
  return tr("Click to place marking-curve points; Enter to commit, Backspace to undo a point, Esc "
            "to cancel");
}

} // namespace roadmaker::editor
