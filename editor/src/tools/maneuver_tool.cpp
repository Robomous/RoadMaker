#include "tools/maneuver_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/projection.hpp"

namespace roadmaker::editor {

namespace {

/// Height [m] the overlay floats above the pavement it describes, so the paths
/// and their handles read against the floor instead of z-fighting it. Matched to
/// the junction-surface overlay lift.
constexpr double kManeuverOverlayLift = 0.05;

void append_segment(std::vector<double>& lines,
                    const std::array<double, 3>& a,
                    const std::array<double, 3>& b) {
  lines.insert(lines.end(),
               {a[0], a[1], a[2] + kManeuverOverlayLift, b[0], b[1], b[2] + kManeuverOverlayLift});
}

void append_path(std::vector<double>& lines, const std::vector<std::array<double, 3>>& path) {
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    append_segment(lines, path[i], path[i + 1]);
  }
}

/// The elevation of the path sample nearest (x, y) — what a handle sitting on
/// (or beside) the path should float above. A junction path is short and
/// coarsely sampled, so the linear scan is free.
double path_z(const JunctionManeuverInfo& info, double x, double y) {
  double best_z = 0.0;
  double best_d = std::numeric_limits<double>::infinity();
  for (const std::array<double, 3>& sample : info.path) {
    const double d = std::hypot(sample[0] - x, sample[1] - y);
    if (d < best_d) {
      best_d = d;
      best_z = sample[2];
    }
  }
  return best_z;
}

/// Unit direction of a slide's +t axis, and the axis length per metre of
/// offset. `anchor` sits at offset 0, `min_point`/`max_point` at the bounds, so
/// the direction is the bound-to-bound vector normalized by the offset span.
std::optional<std::array<double, 2>> slide_direction(const ManeuverSlide& slide) {
  const double span = slide.max_offset - slide.min_offset;
  if (std::abs(span) <= 1e-9) {
    return std::nullopt; // a zero-width anchor lane: nothing to slide along
  }
  const std::array<double, 2> dir{(slide.max_point[0] - slide.min_point[0]) / span,
                                  (slide.max_point[1] - slide.min_point[1]) / span};
  if (std::hypot(dir[0], dir[1]) <= 1e-9) {
    return std::nullopt;
  }
  return dir;
}

/// World position of a slide at `offset` — the anchor walked along the +t axis.
std::array<double, 2> slide_point(const ManeuverSlide& slide, double offset) {
  const std::optional<std::array<double, 2>> dir = slide_direction(slide);
  if (!dir.has_value()) {
    return slide.anchor;
  }
  return {slide.anchor[0] + ((*dir)[0] * offset), slide.anchor[1] + ((*dir)[1] * offset)};
}

/// The offset the cursor asks for, clamped to the slide's OWN bounds. The
/// bounds are read off the query and never recomputed — the anchor lane's span
/// is the kernel's business (p4-s6).
double slide_offset(const ManeuverSlide& slide, double x, double y) {
  const std::optional<std::array<double, 2>> dir = slide_direction(slide);
  if (!dir.has_value()) {
    return 0.0;
  }
  const double length_sq = ((*dir)[0] * (*dir)[0]) + ((*dir)[1] * (*dir)[1]);
  const double along =
      (((x - slide.anchor[0]) * (*dir)[0]) + ((y - slide.anchor[1]) * (*dir)[1])) / length_sq;
  return std::clamp(along, slide.min_offset, slide.max_offset);
}

} // namespace

ManeuverTool::ManeuverTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: every id the tool is holding becomes
  // stale (and can alias a fresh entity), so drop the lot.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
  // The junction under attention comes from the selection — inspecting "the
  // junction you already selected" is how every junction tool starts.
  connect(&selection_, &SelectionModel::selection_changed, this, [this] { sync_to_selection(); });
}

void ManeuverTool::activate() {
  sync_to_selection();
}

void ManeuverTool::deactivate() {
  if (press_.has_value() && document_.preview_active()) {
    document_.cancel_preview();
  }
  reset_all();
}

void ManeuverTool::reset_all() {
  const bool had_active = active_.has_value();
  inspected_ = JunctionId{};
  active_.reset();
  hovered_.reset();
  hovered_handle_ = {};
  focused_point_.reset();
  press_.reset();
  if (had_active) {
    emit maneuver_selection_changed();
  }
  emit preview_changed();
}

void ManeuverTool::sync_to_selection() {
  if (syncing_) {
    return; // our own mirror coming back round
  }
  const RoadNetwork& network = document_.network();

  // A junction selection names the junction outright; a CONNECTING-ROAD
  // selection names it too (Road::junction), which is what lets the maneuver be
  // selected as an ordinary road everywhere else in the editor.
  JunctionId junction = selection_.primary().junction;
  if (!junction.is_valid()) {
    if (const Road* road = network.road(selection_.primary().road); road != nullptr) {
      junction = road->junction;
    }
  }
  // junction_maneuvers() is empty for a stale id AND for a junction with no
  // connections at all (a span/virtual junction), so it is the whole
  // eligibility test.
  const std::vector<JunctionManeuverInfo> all = junction.is_valid()
                                                    ? junction_maneuvers(network, junction)
                                                    : std::vector<JunctionManeuverInfo>{};
  const JunctionId next = all.empty() ? JunctionId{} : junction;

  // The active maneuver follows any selected road that IS one of them, so a
  // scene-tree click on a connecting road lands on the same sub-selection a
  // viewport click would.
  std::optional<ActiveManeuver> active;
  if (next.is_valid()) {
    for (const RoadId road : selection_.selected_roads()) {
      const auto match = std::ranges::find_if(
          all, [road](const JunctionManeuverInfo& info) { return info.road == road; });
      if (match != all.end()) {
        active = ActiveManeuver{.junction = next, .road = road};
        break;
      }
    }
    if (active_.has_value() && active_->junction == next && !active.has_value()) {
      // Keep a sub-selection the selection change did not contradict (selecting
      // the junction itself must not drop the maneuver being worked on).
      const auto match = std::ranges::find_if(
          all, [this](const JunctionManeuverInfo& info) { return info.road == active_->road; });
      if (match != all.end()) {
        active = active_;
      }
    }
  }

  if (next == inspected_ && active == active_) {
    return;
  }
  inspected_ = next;
  const bool active_changed = active != active_;
  active_ = active;
  hovered_.reset();
  hovered_handle_ = {};
  focused_point_.reset();
  if (active_changed) {
    emit maneuver_selection_changed();
  }
  emit preview_changed();
}

std::vector<JunctionManeuverInfo> ManeuverTool::maneuvers() const {
  return inspected_.is_valid() ? junction_maneuvers(document_.network(), inspected_)
                               : std::vector<JunctionManeuverInfo>{};
}

std::optional<JunctionManeuverInfo> ManeuverTool::active_maneuver_info() const {
  if (!active_.has_value()) {
    return std::nullopt;
  }
  for (const JunctionManeuverInfo& info : maneuvers()) {
    if (info.road == active_->road) {
      return info;
    }
  }
  return std::nullopt;
}

double ManeuverTool::tolerance(const ToolEvent& event) const {
  return event.screen.has_value() ? screen_pick_radius() : pick_radius_;
}

std::optional<double> ManeuverTool::cursor_distance(const ToolEvent& event,
                                                    const std::array<double, 3>& point) const {
  if (!event.screen.has_value()) {
    return std::hypot(point[0] - event.world_x, point[1] - event.world_y);
  }
  const ScreenContext& screen = *event.screen;
  const std::optional<std::array<double, 2>> projected =
      project_to_screen(screen.camera, point[0], point[1], point[2], screen.width, screen.height);
  if (!projected.has_value()) {
    return std::nullopt; // behind the camera
  }
  return std::hypot((*projected)[0] - screen.px, (*projected)[1] - screen.py);
}

JunctionId ManeuverTool::resolve_junction(const ToolEvent& event) const {
  if (event.pick.has_value()) {
    if (event.pick->junction.is_valid()) {
      return event.pick->junction; // the floor names its junction outright
    }
    if (const Road* road = document_.network().road(event.pick->road); road != nullptr) {
      if (road->junction.is_valid()) {
        return road->junction; // a connecting road names the junction it belongs to
      }
    }
  }
  return inspected_;
}

std::optional<RoadId> ManeuverTool::maneuver_under(const ToolEvent& event) const {
  const double limit = tolerance(event);
  std::optional<RoadId> best;
  double best_distance = limit;
  for (const JunctionManeuverInfo& info : maneuvers()) {
    if (info.path.size() < 2) {
      continue;
    }
    std::optional<double> distance;
    if (event.screen.has_value()) {
      if (const std::optional<PolylineScreenHit> hit =
              screen_distance_to_polyline(*event.screen, info.path)) {
        distance = hit->distance;
      }
    } else {
      // Headless fallback: the same nearest-approach test in plan-view metres.
      double best_world = std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i + 1 < info.path.size(); ++i) {
        const std::array<double, 3>& a = info.path[i];
        const std::array<double, 3>& b = info.path[i + 1];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double length_sq = (dx * dx) + (dy * dy);
        const double t =
            length_sq <= 1e-12
                ? 0.0
                : std::clamp((((event.world_x - a[0]) * dx) + ((event.world_y - a[1]) * dy)) /
                                 length_sq,
                             0.0,
                             1.0);
        best_world =
            std::min(best_world,
                     std::hypot(a[0] + (dx * t) - event.world_x, a[1] + (dy * t) - event.world_y));
      }
      distance = best_world;
    }
    if (distance.has_value() && *distance <= best_distance) {
      best_distance = *distance;
      best = info.road;
    }
  }
  return best;
}

ManeuverHandleRef ManeuverTool::pick_handle(const JunctionManeuverInfo& info,
                                            const ToolEvent& event) const {
  const double limit = tolerance(event);
  ManeuverHandleRef best;
  double best_distance = limit;
  const auto consider = [&](ManeuverHandle kind, std::size_t index, std::array<double, 3> at) {
    const std::optional<double> distance = cursor_distance(event, at);
    if (distance.has_value() && *distance <= best_distance) {
      best_distance = *distance;
      best = ManeuverHandleRef{.kind = kind, .index = index};
    }
  };

  // Endpoints and control points first: an insert marker sitting between two of
  // them must never steal a grab from the thing it sits between.
  const std::array<double, 2> start = slide_point(info.start_slide, info.start_offset);
  const std::array<double, 2> end = slide_point(info.end_slide, info.end_offset);
  consider(ManeuverHandle::Start, 0, {start[0], start[1], path_z(info, start[0], start[1])});
  consider(ManeuverHandle::End, 0, {end[0], end[1], path_z(info, end[0], end[1])});
  for (std::size_t i = 0; i < info.control_points.size(); ++i) {
    const Waypoint& point = info.control_points[i];
    consider(ManeuverHandle::Point, i, {point.x, point.y, path_z(info, point.x, point.y)});
  }
  if (best.kind != ManeuverHandle::None) {
    return best;
  }

  // Insert markers: the midpoints of the effective chain
  // [start, control points…, end]. Marker i inserts BEFORE control point i.
  std::vector<std::array<double, 2>> chain;
  chain.reserve(info.control_points.size() + 2);
  chain.push_back(start);
  for (const Waypoint& point : info.control_points) {
    chain.push_back({point.x, point.y});
  }
  chain.push_back(end);
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    const std::array<double, 2> mid{(chain[i][0] + chain[i + 1][0]) / 2.0,
                                    (chain[i][1] + chain[i + 1][1]) / 2.0};
    consider(ManeuverHandle::Insert, i, {mid[0], mid[1], path_z(info, mid[0], mid[1])});
  }
  return best;
}

void ManeuverTool::set_active(std::optional<ActiveManeuver> maneuver) {
  const bool changed = maneuver != active_;
  active_ = std::move(maneuver);
  focused_point_.reset();
  if (active_.has_value()) {
    // Mirror BOTH: the connecting road (so a maneuver is a selectable,
    // highlightable entity like any other road) and its junction LAST, so the
    // Properties pane's primary entry stays the junction whose Maneuvers rows
    // the author is reading.
    const std::array<SelectionEntry, 2> entries{
        SelectionEntry{.road = active_->road, .lane = LaneId{}},
        SelectionEntry{.junction = active_->junction}};
    syncing_ = true;
    selection_.select_many(entries, SelectMode::Replace);
    syncing_ = false;
  }
  if (changed) {
    emit maneuver_selection_changed();
  }
  emit preview_changed();
}

void ManeuverTool::select_maneuver(RoadId road) {
  if (!inspected_.is_valid()) {
    return;
  }
  const std::vector<JunctionManeuverInfo> all = maneuvers();
  const bool known = std::ranges::any_of(
      all, [road](const JunctionManeuverInfo& info) { return info.road == road; });
  set_active(
      known ? std::optional<ActiveManeuver>(ActiveManeuver{.junction = inspected_, .road = road})
            : std::nullopt);
}

bool ManeuverTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || press_.has_value()) {
    return false;
  }

  // The active maneuver's handles win over re-resolving a maneuver: they sit ON
  // the path they belong to.
  if (active_.has_value()) {
    if (const std::optional<JunctionManeuverInfo> info = active_maneuver_info()) {
      ManeuverHandleRef handle = pick_handle(*info, event);
      if (handle.kind != ManeuverHandle::None) {
        std::vector<Waypoint> points = info->control_points;
        if (handle.kind == ManeuverHandle::Insert) {
          // The insert and the drag that follows it are ONE command: the point
          // is born where the marker was and moves with the cursor from there.
          points.insert(points.begin() + static_cast<std::ptrdiff_t>(handle.index),
                        Waypoint{.x = event.world_x, .y = event.world_y});
          handle = ManeuverHandleRef{.kind = ManeuverHandle::Point, .index = handle.index};
        }
        if (handle.kind == ManeuverHandle::Point) {
          focused_point_ = handle.index;
        }
        // Armed, not previewing: a press that never moves must leave the undo
        // stack alone, so the session only opens on the first move.
        press_ = PressState{.handle = handle, .info = *info, .points = std::move(points)};
        emit preview_changed();
        return true;
      }
    }
  }

  const JunctionId junction = resolve_junction(event);
  if (junction.is_valid() && junction != inspected_) {
    selection_.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
    sync_to_selection();
  }
  if (!inspected_.is_valid()) {
    return false; // nothing here — let the viewport keep the click
  }
  if (const std::optional<RoadId> road = maneuver_under(event)) {
    set_active(ActiveManeuver{.junction = inspected_, .road = *road});
    emit status_message(tr("Drag a point to reshape this turn, a midpoint to insert one, or an "
                           "endpoint to slide it across the arm · Del removes a point"));
    return true;
  }
  set_active(std::nullopt);
  return false;
}

bool ManeuverTool::mouse_move(const ToolEvent& event) {
  if (press_.has_value()) {
    update_drag(event);
    return true;
  }

  std::optional<RoadId> hovered = maneuver_under(event);
  ManeuverHandleRef handle;
  if (active_.has_value()) {
    if (const std::optional<JunctionManeuverInfo> info = active_maneuver_info()) {
      handle = pick_handle(*info, event);
    }
  }
  if (handle.kind == ManeuverHandle::Point) {
    focused_point_ = handle.index;
  }
  if (hovered != hovered_ || handle != hovered_handle_) {
    hovered_ = hovered;
    hovered_handle_ = handle;
    emit preview_changed();
  }
  return false; // hovering never consumes: camera nav and the readout stay live
}

std::vector<Waypoint> ManeuverTool::points_for(double world_x, double world_y) const {
  std::vector<Waypoint> points = press_->points;
  if (press_->handle.kind == ManeuverHandle::Point && press_->handle.index < points.size()) {
    points[press_->handle.index] = Waypoint{.x = world_x, .y = world_y};
  }
  return points;
}

std::optional<double> ManeuverTool::start_offset_for(double world_x, double world_y) const {
  // Always supplied, dragged or not: one command carries the whole authored
  // state, so the release is a single undo entry however the gesture started.
  return press_->handle.kind == ManeuverHandle::Start
             ? slide_offset(press_->info.start_slide, world_x, world_y)
             : press_->info.start_offset;
}

std::optional<double> ManeuverTool::end_offset_for(double world_x, double world_y) const {
  return press_->handle.kind == ManeuverHandle::End
             ? slide_offset(press_->info.end_slide, world_x, world_y)
             : press_->info.end_offset;
}

void ManeuverTool::update_drag(const ToolEvent& event) {
  if (!active_.has_value()) {
    return;
  }
  const ActiveManeuver maneuver = *active_;
  const std::vector<Waypoint> points = points_for(event.world_x, event.world_y);
  const std::optional<double> start = start_offset_for(event.world_x, event.world_y);
  const std::optional<double> end = end_offset_for(event.world_x, event.world_y);

  // A gesture that asks for the state it started from must author NOTHING —
  // and `set_maneuver_path` cannot be relied on to refuse it, because on a
  // still-derived maneuver an unchanged path still flips the implicit lock and
  // succeeds (WP4, #227). So the tool refuses it here: a click that wobbles a
  // pixel, or a drag returned to where it began, leaves the undo stack alone.
  if (points == press_->info.control_points && start == press_->info.start_offset &&
      end == press_->info.end_offset) {
    if (document_.preview_active()) {
      document_.cancel_preview(); // a drag walked back to its starting shape
      emit preview_changed();
    }
    return;
  }

  const Document::PreviewFactory factory = [maneuver, points, start, end](
                                               const RoadNetwork& network) {
    return edit::set_maneuver_path(network, maneuver.junction, maneuver.road, points, start, end);
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

bool ManeuverTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!press_.has_value()) {
    return false;
  }
  // A press that never moved never opened a session, so this commits nothing.
  const bool edited = document_.preview_active();
  document_.commit_preview();
  press_.reset();
  if (edited) {
    emit status_message(tr("Maneuver reshaped and locked — Ctrl+Z to undo"));
    emit maneuver_selection_changed();
  }
  emit preview_changed();
  return true;
}

bool ManeuverTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    if (press_.has_value()) {
      if (document_.preview_active()) {
        document_.cancel_preview();
      }
      press_.reset();
      emit status_message(tr("Maneuver edit cancelled"));
      emit preview_changed();
      return true;
    }
    if (active_.has_value()) {
      set_active(std::nullopt);
      return true;
    }
    return false;
  }

  if (key != Qt::Key_Delete && key != Qt::Key_Backspace) {
    return false;
  }
  if (press_.has_value() || !focused_point_.has_value()) {
    return false;
  }
  const std::optional<JunctionManeuverInfo> info = active_maneuver_info();
  if (!info.has_value() || *focused_point_ >= info->control_points.size()) {
    return false;
  }
  std::vector<Waypoint> points = info->control_points;
  points.erase(points.begin() + static_cast<std::ptrdiff_t>(*focused_point_));
  if (!document_
           .push_command(edit::set_maneuver_path(document_.network(),
                                                 active_->junction,
                                                 active_->road,
                                                 points,
                                                 info->start_offset,
                                                 info->end_offset))
           .has_value()) {
    emit toast_requested(tr("That point could not be removed"), ToastSeverity::Warning);
    return true;
  }
  focused_point_.reset();
  emit status_message(tr("Point removed — Ctrl+Z to undo"));
  emit maneuver_selection_changed();
  emit preview_changed();
  return true;
}

PreviewGeometry ManeuverTool::preview() const {
  PreviewGeometry geometry;
  const std::vector<JunctionManeuverInfo> all = maneuvers();
  std::optional<JunctionManeuverInfo> active_info;
  for (const JunctionManeuverInfo& info : all) {
    const bool active = active_.has_value() && active_->road == info.road;
    if (active) {
      active_info = info;
      append_path(geometry.line_positions, info.path);
      continue;
    }
    // Context: every other turn of the junction, dashed so the one under
    // attention is unmistakable.
    append_path(geometry.dashed_line_positions, info.path);
  }
  if (!active_info.has_value()) {
    return geometry;
  }

  const auto handle_state = [this](ManeuverHandle kind, std::size_t index) {
    const ManeuverHandleRef ref{.kind = kind, .index = index};
    if (press_.has_value() && press_->handle == ref) {
      return HandleState::Grabbed;
    }
    if (hovered_handle_ == ref) {
      return HandleState::Hovered;
    }
    if (kind == ManeuverHandle::Point && focused_point_ == index) {
      return HandleState::Hovered; // the point Del would remove
    }
    return HandleState::Idle;
  };

  // The two slide segments, dashed: they are the constraint lines the endpoints
  // may travel along, not pavement.
  for (const ManeuverSlide& slide : {active_info->start_slide, active_info->end_slide}) {
    const double z = path_z(*active_info, slide.anchor[0], slide.anchor[1]);
    append_segment(geometry.dashed_line_positions,
                   {slide.min_point[0], slide.min_point[1], z},
                   {slide.max_point[0], slide.max_point[1], z});
  }

  const std::array<double, 2> start =
      slide_point(active_info->start_slide, active_info->start_offset);
  const std::array<double, 2> end = slide_point(active_info->end_slide, active_info->end_offset);
  geometry.add_handle(start[0],
                      start[1],
                      path_z(*active_info, start[0], start[1]) + kManeuverOverlayLift,
                      HandleKind::Sample,
                      handle_state(ManeuverHandle::Start, 0));
  geometry.add_handle(end[0],
                      end[1],
                      path_z(*active_info, end[0], end[1]) + kManeuverOverlayLift,
                      HandleKind::Sample,
                      handle_state(ManeuverHandle::End, 0));

  for (std::size_t i = 0; i < active_info->control_points.size(); ++i) {
    const Waypoint& point = active_info->control_points[i];
    geometry.add_handle(point.x,
                        point.y,
                        path_z(*active_info, point.x, point.y) + kManeuverOverlayLift,
                        HandleKind::Node,
                        handle_state(ManeuverHandle::Point, i));
  }

  std::vector<std::array<double, 2>> chain;
  chain.reserve(active_info->control_points.size() + 2);
  chain.push_back(start);
  for (const Waypoint& point : active_info->control_points) {
    chain.push_back({point.x, point.y});
  }
  chain.push_back(end);
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    const std::array<double, 2> mid{(chain[i][0] + chain[i + 1][0]) / 2.0,
                                    (chain[i][1] + chain[i + 1][1]) / 2.0};
    geometry.add_handle(mid[0],
                        mid[1],
                        path_z(*active_info, mid[0], mid[1]) + kManeuverOverlayLift,
                        HandleKind::Midpoint,
                        handle_state(ManeuverHandle::Insert, i));
  }
  return geometry;
}

QString ManeuverTool::instruction() const {
  if (!inspected_.is_valid()) {
    return tr("Select a junction to work on its turns");
  }
  if (!active_.has_value()) {
    return tr("Click a turn to reshape it · the Properties pane sets its turn type and lock");
  }
  return tr("Drag a point to reshape the turn, a midpoint to insert one, an endpoint to slide it "
            "across the arm · Del removes a point · Esc cancels");
}

} // namespace roadmaker::editor
