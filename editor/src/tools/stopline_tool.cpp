#include "tools/stopline_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// Distance from (x, y) to the segment a–b — the band's hit-test metric. The
/// band is thin and wide, so its CENTRELINE is what the cursor grabs; a point
/// test against the midpoint would make a wide arm nearly unclickable.
double distance_to_segment(const std::array<double, 2>& a,
                           const std::array<double, 2>& b,
                           double x,
                           double y) {
  const double dx = b[0] - a[0];
  const double dy = b[1] - a[1];
  const double len2 = (dx * dx) + (dy * dy);
  if (len2 <= 1e-18) {
    return std::hypot(a[0] - x, a[1] - y);
  }
  const double t = std::clamp((((x - a[0]) * dx) + ((y - a[1]) * dy)) / len2, 0.0, 1.0);
  return std::hypot(a[0] + (t * dx) - x, a[1] + (t * dy) - y);
}

std::array<double, 2> midpoint(const JunctionStopLineInfo& info) {
  return {(info.left[0] + info.right[0]) / 2.0, (info.left[1] + info.right[1]) / 2.0};
}

} // namespace

StopLineTool::StopLineTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: every JunctionId the tool is holding
  // becomes stale (and can alias a fresh junction), so drop the lot.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
}

void StopLineTool::activate() {}

void StopLineTool::deactivate() {
  if (press_.has_value() && document_.preview_active()) {
    document_.cancel_preview();
  }
  reset_all();
}

void StopLineTool::reset_all() {
  const bool had_active = active_.has_value();
  press_.reset();
  active_.reset();
  hovered_.reset();
  if (had_active) {
    emit stopline_selection_changed();
  }
  emit preview_changed();
}

std::optional<JunctionStopLineInfo> StopLineTool::solve(const ActiveStopLine& line) const {
  for (const JunctionStopLineInfo& info : junction_stoplines(document_.network(), line.junction)) {
    if (info.arm == line.arm) {
      return info;
    }
  }
  return std::nullopt;
}

std::optional<JunctionStopLineInfo> StopLineTool::active_stopline_info() const {
  return active_.has_value() ? solve(*active_) : std::nullopt;
}

std::optional<ActiveStopLine> StopLineTool::resolve_stopline(const ToolEvent& event) const {
  const RoadNetwork& network = document_.network();
  std::optional<ActiveStopLine> best;
  double best_distance = pick_radius_;
  network.for_each_junction([&](JunctionId id, const Junction&) {
    for (const JunctionStopLineInfo& info : junction_stoplines(network, id)) {
      const double d = distance_to_segment(info.left, info.right, event.world_x, event.world_y);
      if (d <= best_distance) {
        best_distance = d;
        best = ActiveStopLine{.junction = id, .arm = info.arm};
      }
    }
  });
  return best;
}

std::optional<double> StopLineTool::distance_for(const ActiveStopLine& line,
                                                 const JunctionStopLineInfo& info,
                                                 double world_x,
                                                 double world_y) const {
  const Road* road = document_.network().road(line.arm.road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  // The band slides ALONG the arm, so the cursor's station is the whole input;
  // lateral travel is ignored (as the Corner tool ignores off-bisector travel).
  const double s = find_station(road->plan_view, world_x, world_y).s;
  const double half = info.thickness / 2.0;
  // Invert the solve: s_center = distance + half (Start) or
  // length - distance - half (End).
  const double distance =
      line.arm.contact == ContactPoint::Start ? s - half : road->plan_view.length() - s - half;
  return std::clamp(distance, 0.0, info.max_distance);
}

void StopLineTool::set_active(std::optional<ActiveStopLine> line) {
  const bool changed = line != active_;
  active_ = std::move(line);
  if (active_.has_value()) {
    // The rest of the UI follows the junction (there is no stop-line selection
    // entry); Replace so a click reads like every other click.
    selection_.select(SelectionEntry{.junction = active_->junction}, SelectMode::Replace);
  }
  if (changed) {
    emit stopline_selection_changed();
  }
  emit preview_changed();
}

bool StopLineTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || press_.has_value()) {
    return false;
  }

  // Pressing ON the active band arms a drag; the band is its own handle, so
  // there is no separate grab point to miss.
  if (active_.has_value()) {
    if (const std::optional<JunctionStopLineInfo> info = solve(*active_)) {
      if (distance_to_segment(info->left, info->right, event.world_x, event.world_y) <=
          pick_radius_) {
        // Armed, not previewing: a press that never moves must leave the undo
        // stack alone, so the session only starts on the first move.
        press_ = *info;
        emit preview_changed();
        return true;
      }
    }
  }

  if (std::optional<ActiveStopLine> line = resolve_stopline(event)) {
    set_active(std::move(line));
    emit status_message(tr("Drag the stop line to set its distance, or press F to flip it"));
    return true;
  }

  set_active(std::nullopt);
  return false; // nothing here — let the viewport keep the click
}

bool StopLineTool::mouse_move(const ToolEvent& event) {
  if (press_.has_value()) {
    update_drag(event.world_x, event.world_y);
    return true;
  }

  std::optional<ActiveStopLine> hovered = resolve_stopline(event);
  if (hovered != hovered_) {
    hovered_ = std::move(hovered);
    emit preview_changed();
  }
  return false; // hovering never consumes: camera nav and the readout stay live
}

void StopLineTool::update_drag(double world_x, double world_y) {
  if (!active_.has_value()) {
    return;
  }
  const ActiveStopLine line = *active_;
  const std::optional<double> distance = distance_for(line, *press_, world_x, world_y);
  if (!distance.has_value()) {
    return;
  }
  const Document::PreviewFactory factory = [line, value = *distance](const RoadNetwork& network) {
    return edit::set_stopline_distance(network, line.junction, line.arm, value);
  };

  if (!document_.preview_active()) {
    // First move of the gesture opens the session.
    if (!document_.begin_preview(factory(document_.network())).has_value()) {
      return; // still armed: a later move can recover
    }
  } else if (!document_.update_preview(factory).has_value()) {
    return; // session stays at its last good frame
  }
  emit preview_changed();
}

bool StopLineTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!press_.has_value()) {
    return false;
  }
  // A press that never moved never opened a session, so this commits nothing.
  const bool edited = document_.preview_active();
  document_.commit_preview();
  press_.reset();
  if (edited) {
    emit status_message(tr("Stop line distance set — Ctrl+Z to undo"));
  }
  emit preview_changed();
  return true;
}

bool StopLineTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_F) {
    if (!active_.has_value() || press_.has_value()) {
      return false;
    }
    // The command refuses a direction with no lanes to span, so a flip that
    // cannot work reports rather than authoring an empty band.
    if (!document_
             .push_command(
                 edit::flip_stopline(document_.network(), active_->junction, active_->arm))
             .has_value()) {
      emit toast_requested(tr("That direction has no lanes to span"), ToastSeverity::Warning);
      return true;
    }
    emit status_message(tr("Stop line flipped — Ctrl+Z to undo"));
    emit stopline_selection_changed();
    emit preview_changed();
    return true;
  }

  if (key != Qt::Key_Escape) {
    return false;
  }
  if (press_.has_value()) {
    if (document_.preview_active()) {
      document_.cancel_preview();
    }
    press_.reset();
    emit status_message(tr("Stop line edit cancelled"));
    emit preview_changed();
    return true;
  }
  if (active_.has_value()) {
    set_active(std::nullopt);
    return true;
  }
  return false;
}

PreviewGeometry StopLineTool::preview() const {
  PreviewGeometry geometry;

  const auto band = [&geometry](const JunctionStopLineInfo& info) {
    geometry.line_positions.insert(
        geometry.line_positions.end(),
        {info.left[0], info.left[1], 0.0, info.right[0], info.right[1], 0.0});
  };

  std::optional<JunctionStopLineInfo> active_info;
  if (active_.has_value()) {
    active_info = solve(*active_);
    if (active_info.has_value()) {
      band(*active_info);
    }
  }
  if (hovered_.has_value() && hovered_ != active_) {
    if (const std::optional<JunctionStopLineInfo> info = solve(*hovered_)) {
      band(*info);
    }
  }

  if (active_info.has_value()) {
    const std::array<double, 2> at = midpoint(*active_info);
    geometry.add_handle(at[0],
                        at[1],
                        0.0,
                        HandleKind::Node,
                        press_.has_value() ? HandleState::Grabbed : HandleState::Hovered);
  }
  return geometry;
}

QString StopLineTool::instruction() const {
  return tr("Click a junction stop line, then drag it along the arm to set its distance · F flips "
            "it · Esc cancels");
}

} // namespace roadmaker::editor
