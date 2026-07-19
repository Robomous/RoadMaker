#include "tools/prop_curve_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/tol.hpp"

#include <QRandomGenerator>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/prop_placement.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// A prop curve anchors to a road whose reference line passes within this lateral
/// distance [m] of the first click — the same reach the tree drop uses, so aiming
/// on or just beside the carriageway grabs the road.
constexpr double kAnchorSnapThreshold = kObjectSnapThreshold;

/// Spacing keys nudge by this step [m], clamped to [min, max] — a usable spread
/// from tightly packed hedging to sparse avenue trees.
constexpr double kSpacingStep = 0.5;
constexpr double kSpacingMin = 0.5;
constexpr double kSpacingMax = 50.0;

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

} // namespace

PropCurveTool::PropCurveTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection),
      session_seed_(QRandomGenerator::global()->generate()) {}

void PropCurveTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem PropCurveTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void PropCurveTool::deactivate() {
  reset_session();
}

bool PropCurveTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  place_point(event.world_x, event.world_y);
  return true;
}

bool PropCurveTool::mouse_move(const ToolEvent& event) {
  cursor_ = Waypoint{.x = event.world_x, .y = event.world_y};
  emit preview_changed();
  return false; // hovering never consumes: camera nav and hover readout stay live
}

bool PropCurveTool::mouse_double_click(const ToolEvent& event) {
  static_cast<void>(event); // the pair's first press already placed the point
  bake();
  return true;
}

bool PropCurveTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    bake();
    return true;
  }
  if (key == Qt::Key_BracketLeft) {
    adjust_spacing(-kSpacingStep);
    return true;
  }
  if (key == Qt::Key_BracketRight) {
    adjust_spacing(kSpacingStep);
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
    emit status_message(tr("Prop curve cancelled"));
    return true;
  }
  return false;
}

void PropCurveTool::place_point(double world_x, double world_y) {
  if (points_.empty()) {
    // The first point anchors the curve. Refuse if the asset is incompatible or
    // the click is off any road — nothing is placed until an anchor is found.
    if (!is_prop_asset(current_item())) {
      emit toast_requested(tr("Select a tree or shrub in the Library first"),
                           ToastSeverity::Warning);
      return;
    }
    const std::optional<RoadStation> anchor =
        nearest_road_station(document_.network(), world_x, world_y, kAnchorSnapThreshold);
    if (!anchor.has_value()) {
      emit status_message(tr("Click on or beside a road to start the prop curve"));
      return;
    }
    anchor_ = anchor->road;
  }
  points_.push_back(Waypoint{.x = world_x, .y = world_y});
  emit status_message(tr("%n point(s) — Enter or double-click bakes the props",
                         nullptr,
                         static_cast<int>(points_.size())));
  emit preview_changed();
}

void PropCurveTool::adjust_spacing(double delta) {
  spacing_m_ = std::clamp(spacing_m_ + delta, kSpacingMin, kSpacingMax);
  emit status_message(tr("Prop spacing %1 m").arg(spacing_m_, 0, 'f', 1));
  emit preview_changed();
}

void PropCurveTool::bake() {
  if (points_.size() < 2 || !anchor_.has_value()) {
    emit status_message(tr("A prop curve needs at least two points"));
    return;
  }
  // Non-const so the props can be MOVED into add_object below (a const binding
  // would make std::move a redundant copy — GCC's -Werror=redundant-move).
  Expected<PropCurveDistribution> distribution = distribute_props_along_curve(
      document_.network(), *anchor_, points_, current_item(), spacing_m_, session_seed_);
  if (!distribution.has_value()) {
    // Session kept: the points are the user's work; fix and retry.
    emit status_message(tr("Cannot distribute props: %1")
                            .arg(QString::fromStdString(distribution.error().message)));
    return;
  }
  const std::size_t placed = distribution->props.size();
  const std::size_t skipped = distribution->skipped;

  // ONE undo unit: every distributed prop adds together (crosswalk macro
  // pattern), so a single Ctrl+Z removes the whole bake.
  document_.undo_stack()->beginMacro(tr("Bake prop curve"));
  for (auto& [road, object] : distribution->props) {
    (void)document_.push_command(edit::add_object(document_.network(), road, std::move(object)));
  }
  document_.undo_stack()->endMacro();

  reset_session();
  if (skipped > 0) {
    emit status_message(
        tr("Baked %1 props (%2 skipped) — Ctrl+Z to undo").arg(placed).arg(skipped));
  } else {
    emit status_message(tr("Baked %1 props — Ctrl+Z to undo").arg(placed));
  }
}

void PropCurveTool::reset_session() {
  points_.clear();
  anchor_.reset();
  cursor_.reset();
  // A fresh seed for the next curve, so two curves of a mixed set differ.
  session_seed_ = QRandomGenerator::global()->generate();
  emit preview_changed();
}

PreviewGeometry PropCurveTool::preview() const {
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

  // One handle per distributed prop the bake will author (ghost==commit): the
  // placed points plus the cursor as a provisional last point, run through the
  // same fit → sample → project the bake uses. A failed distribution silently
  // leaves just the raw ghost polyline.
  if (anchor_.has_value() && !points_.empty() && is_prop_asset(current_item())) {
    std::vector<Waypoint> candidate = points_;
    if (cursor_.has_value() &&
        std::hypot(cursor_->x - points_.back().x, cursor_->y - points_.back().y) >= tol::kLength) {
      candidate.push_back(*cursor_);
    }
    if (candidate.size() >= 2) {
      const Expected<PropCurveDistribution> distribution = distribute_props_along_curve(
          document_.network(), *anchor_, candidate, current_item(), spacing_m_, session_seed_);
      if (distribution.has_value()) {
        for (const std::array<double, 2>& point : distribution->preview_points) {
          geometry.add_handle(point[0], point[1], 0.0, HandleKind::Node, HandleState::Hovered);
        }
      }
    }
  }

  return geometry;
}

QString PropCurveTool::instruction() const {
  return tr("Click to place prop-curve points; Enter bakes, [ / ] set spacing (%1 m), "
            "Backspace undoes a point, Esc cancels")
      .arg(spacing_m_, 0, 'f', 1);
}

} // namespace roadmaker::editor
