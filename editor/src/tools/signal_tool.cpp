#include "tools/signal_tool.hpp"

#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/signal_placement.hpp"

namespace roadmaker::editor {

namespace {

/// Height [m] the overlay floats above the pavement it describes, matched to
/// the maneuver overlay so the two read as one layer when both are up.
constexpr double kSignalOverlayLift = 0.05;

/// Cursor travel [m] past which an armed placement becomes a drag (the
/// PropPointTool tolerance).
constexpr double kDragTolerance = 0.5;

void append_segment(std::vector<double>& lines,
                    const std::array<double, 3>& a,
                    const std::array<double, 3>& b) {
  lines.insert(lines.end(),
               {a[0], a[1], a[2] + kSignalOverlayLift, b[0], b[1], b[2] + kSignalOverlayLift});
}

/// World position of a signal: its (s, t) on its owning road, lifted by its own
/// zOffset — the same projection the mesh builder instances the head at, so the
/// overlay never drifts from the rendered pole.
std::optional<std::array<double, 3>> signal_world(const RoadNetwork& network, SignalId id) {
  const Signal* signal = network.signal(id);
  if (signal == nullptr) {
    return std::nullopt;
  }
  const Road* road = network.road(signal->road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  const std::array<double, 2> plan = station_to_world(road->plan_view, signal->s, signal->t);
  return std::array<double, 3>{plan[0], plan[1], signal->z_offset};
}

/// The mid sample of a maneuver's path — where a gating leader points.
std::optional<std::array<double, 3>> maneuver_midpoint(const std::vector<JunctionManeuverInfo>& all,
                                                       RoadId road) {
  for (const JunctionManeuverInfo& info : all) {
    if (info.road == road && !info.path.empty()) {
      return info.path[info.path.size() / 2];
    }
  }
  return std::nullopt;
}

} // namespace

std::string_view signalize_template_token(edit::SignalizeTemplate tmpl) {
  // The enum and kSignalizationTemplates are deliberately one-to-one and in the
  // same order (road/junction.hpp) — the index IS the mapping, so a template
  // added to one without the other fails to compile here rather than persisting
  // a wrong token.
  const auto index = static_cast<std::size_t>(tmpl);
  return index < std::size(kSignalizationTemplates) ? kSignalizationTemplates[index]
                                                    : std::string_view{};
}

std::optional<edit::SignalizeTemplate> signalize_template_from_token(std::string_view token) {
  for (std::size_t i = 0; i < std::size(kSignalizationTemplates); ++i) {
    if (kSignalizationTemplates[i] == token) {
      return static_cast<edit::SignalizeTemplate>(i);
    }
  }
  return std::nullopt;
}

SignalTool::SignalTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: every id the tool holds becomes stale
  // (and can alias a fresh entity), so drop the lot.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
  connect(&selection_, &SelectionModel::selection_changed, this, [this] { sync_to_selection(); });
  // A command (from the panel, the context menu, or undo) changes what is
  // applied without touching the selection — the rows must re-read it.
  connect(&document_, &Document::mesh_changed, this, [this] { emit signalization_changed(); });
}

void SignalTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem SignalTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void SignalTool::activate() {
  sync_to_selection();
}

void SignalTool::deactivate() {
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  reset_all();
}

void SignalTool::reset_all() {
  inspected_ = JunctionId{};
  press_.reset();
  hover_.reset();
  emit signalization_changed();
  emit preview_changed();
}

void SignalTool::sync_to_selection() {
  if (syncing_) {
    return; // our own mirror coming back round
  }
  const RoadNetwork& network = document_.network();
  JunctionId junction = selection_.primary().junction;
  if (!junction.is_valid()) {
    if (const Road* road = network.road(selection_.primary().road); road != nullptr) {
      junction = road->junction;
    }
  }
  if (!junction.is_valid() || network.junction(junction) == nullptr) {
    return; // a head or a plain road selection leaves the target alone
  }
  set_inspected(junction);
}

void SignalTool::set_inspected(JunctionId junction) {
  if (junction == inspected_) {
    return;
  }
  inspected_ = junction;
  emit signalization_changed();
  emit preview_changed();
}

std::vector<JunctionApproachInfo> SignalTool::approaches() const {
  return inspected_.is_valid() ? junction_signals(document_.network(), inspected_)
                               : std::vector<JunctionApproachInfo>{};
}

void SignalTool::set_pending_template(edit::SignalizeTemplate tmpl) {
  if (pending_.tmpl == tmpl) {
    return;
  }
  pending_.tmpl = tmpl;
  emit signalization_changed();
}

void SignalTool::set_pending_mount_model(std::string model) {
  if (pending_.mount_model == model) {
    return;
  }
  pending_.mount_model = std::move(model);
  emit signalization_changed();
}

std::optional<edit::SignalizeTemplate> SignalTool::applied_template() const {
  const Junction* junction = document_.network().junction(inspected_);
  if (junction == nullptr || junction->signalization.tmpl.empty()) {
    return std::nullopt;
  }
  return signalize_template_from_token(junction->signalization.tmpl);
}

QString SignalTool::signalize_blocker() const {
  const Junction* junction = document_.network().junction(inspected_);
  if (junction == nullptr) {
    return tr("Select a junction to signalize");
  }
  if (!junction->spans.empty()) {
    // asam.net:xodr:1.9.0:junctions.virtual.no_controllers — a virtual junction
    // never carries controllers, so there is nothing to signalize.
    return tr("A span (virtual) junction can't carry traffic lights (ASAM §12.7)");
  }
  if (junction->arms.empty()) {
    return tr("This junction came from another file and has no arms — recreate it to signalize it");
  }
  if (junction->signalization.tmpl == signalize_template_token(pending_.tmpl) &&
      junction->signalization.mount_model == pending_.mount_model) {
    return tr("This junction already has exactly this signalization");
  }
  if (approaches().empty()) {
    return tr("This junction has no approaches to signalize");
  }
  return {};
}

bool SignalTool::can_signalize() const {
  return signalize_blocker().isEmpty();
}

bool SignalTool::can_clear() const {
  const Junction* junction = document_.network().junction(inspected_);
  if (junction == nullptr) {
    return false;
  }
  // Exactly what clear_signalization derives its work from: a synchronization
  // group, a mount record, or a recorded template.
  return !junction->junction_controllers.empty() || !junction->signal_mounts.empty() ||
         !junction->signalization.tmpl.empty();
}

bool SignalTool::signalize() {
  if (!can_signalize()) {
    emit toast_requested(signalize_blocker(), ToastSeverity::Warning);
    return false;
  }
  if (!document_.push_command(edit::signalize_junction(document_.network(), inspected_, pending_))
           .has_value()) {
    emit toast_requested(tr("That template couldn't be applied to this junction"),
                         ToastSeverity::Warning);
    return false;
  }
  emit status_message(tr("Junction signalized — Ctrl+Z to undo"));
  emit signalization_changed();
  emit preview_changed();
  return true;
}

bool SignalTool::clear() {
  if (!can_clear()) {
    emit toast_requested(tr("This junction has no signalization to clear"), ToastSeverity::Warning);
    return false;
  }
  if (!document_.push_command(edit::clear_signalization(document_.network(), inspected_))
           .has_value()) {
    emit toast_requested(tr("That signalization couldn't be cleared"), ToastSeverity::Warning);
    return false;
  }
  emit status_message(tr("Signalization cleared — Ctrl+Z to undo"));
  emit signalization_changed();
  emit preview_changed();
  return true;
}

JunctionId SignalTool::resolve_junction(const ToolEvent& event) const {
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

bool SignalTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || press_.has_value()) {
    return false;
  }

  // A head under the cursor is selected, not replaced: the existing signal
  // selection path already drives the Attributes pane's Signal section.
  if (event.pick.has_value() && event.pick->signal.is_valid()) {
    selection_.select(SelectionEntry{.road = event.pick->road, .signal = event.pick->signal},
                      SelectMode::Replace);
    emit status_message(tr("Signal selected — the Attributes pane edits its pose"));
    emit preview_changed();
    return true;
  }

  // A junction floor (or one of its connecting roads) makes it the target.
  const JunctionId junction = resolve_junction(event);
  if (junction.is_valid() && junction != inspected_) {
    syncing_ = true;
    selection_.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
    syncing_ = false;
    set_inspected(junction);
    emit status_message(
        tr("Junction targeted — the Attributes pane's Signalization group applies a template"));
    return true;
  }

  // Anything else on or beside a road arms a placement, resolved on release. A
  // press in open space arms nothing and is left to the viewport (OpenDRIVE has
  // no world-placed signal, so there is nothing to place there).
  const std::optional<RoadStation> placement =
      nearest_signal_station(document_.network(), event.world_x, event.world_y);
  if (!placement.has_value()) {
    return false;
  }
  press_ = PressState{.world_x = event.world_x,
                      .world_y = event.world_y,
                      .placement = placement,
                      .dragging = false};
  return true;
}

bool SignalTool::mouse_move(const ToolEvent& event) {
  if (press_.has_value()) {
    const bool beyond = std::abs(event.world_x - press_->world_x) > kDragTolerance ||
                        std::abs(event.world_y - press_->world_y) > kDragTolerance;
    if (beyond && press_->placement.has_value()) {
      update_drag(event);
    }
    return true;
  }
  // Plain hover: ghost where a click would land. Never consumes, so the
  // viewport's hover readout and camera navigation stay live.
  hover_ = nearest_signal_station(document_.network(), event.world_x, event.world_y);
  emit preview_changed();
  return false;
}

void SignalTool::update_drag(const ToolEvent& event) {
  const LibraryItem item = current_item();
  if (!is_signal_asset(item)) {
    return; // nothing to preview; the release toasts
  }
  const std::optional<RoadStation> station =
      nearest_signal_station(document_.network(), event.world_x, event.world_y);
  if (!station.has_value()) {
    return; // off every road: hold the last good frame
  }
  press_->placement = station;
  press_->dragging = true;
  const QString tag = item.signal;
  // The factory runs against the BASE network on every frame, so the same
  // add_signal is simply replaced — one command, one undo entry on release.
  const Document::PreviewFactory factory = [tag, station](const RoadNetwork& base) {
    return edit::add_signal(
        base, station->road, make_signal(tag, next_signal_odr_id(base), station->s, station->t));
  };
  if (!document_.preview_active()) {
    if (!document_.begin_preview(factory(document_.network())).has_value()) {
      return; // still armed: a later move can recover
    }
  } else if (!document_.update_preview(factory).has_value()) {
    return; // session stays at its last good frame
  }
  emit preview_changed();
}

void SignalTool::place_signal(const RoadStation& placement) {
  const LibraryItem item = current_item();
  if (!is_signal_asset(item)) {
    emit toast_requested(tr("Select a signal in the Library to place one"), ToastSeverity::Warning);
    return;
  }
  Signal signal =
      make_signal(item.signal, next_signal_odr_id(document_.network()), placement.s, placement.t);
  const std::string odr = signal.odr_id;
  const RoadId road = placement.road;
  if (!document_.push_command(edit::add_signal(document_.network(), road, std::move(signal)))
           .has_value()) {
    emit status_message(tr("Couldn't place a signal here"));
    return;
  }
  // add_signal mints the SignalId only on apply, so look the head up by its
  // odr id on the owning road.
  document_.network().for_each_signal([&](SignalId id, const Signal& placed) {
    if (placed.road == road && placed.odr_id == odr) {
      selection_.select(SelectionEntry{.road = road, .signal = id}, SelectMode::Replace);
    }
  });
  hover_.reset();
  emit status_message(tr("Signal placed — Ctrl+Z to undo"));
  emit preview_changed();
}

bool SignalTool::mouse_release(const ToolEvent& event) {
  if (!press_.has_value()) {
    return false;
  }
  const PressState state = *press_;
  press_.reset();
  if (state.dragging) {
    // The preview session already holds the placement at its last frame:
    // committing it is exactly ONE command.
    const bool placed = document_.preview_active();
    document_.commit_preview();
    if (placed) {
      emit status_message(tr("Signal placed — Ctrl+Z to undo"));
    }
    emit preview_changed();
    return true;
  }
  static_cast<void>(event);
  if (state.placement.has_value()) {
    place_signal(*state.placement);
    return true;
  }
  return false;
}

bool SignalTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape) {
    return false;
  }
  if (press_.has_value()) {
    if (document_.preview_active()) {
      document_.cancel_preview();
    }
    press_.reset();
    emit status_message(tr("Signal placement cancelled"));
    emit preview_changed();
    return true;
  }
  if (inspected_.is_valid()) {
    set_inspected(JunctionId{});
    return true;
  }
  return false;
}

PreviewGeometry SignalTool::preview() const {
  PreviewGeometry geometry;
  const RoadNetwork& network = document_.network();

  // The ghost: where a click would place a head. Suppressed during a drag —
  // the live preview session already shows the real signal.
  if (!press_.has_value() && hover_.has_value()) {
    if (const Road* road = network.road(hover_->road);
        road != nullptr && !road->plan_view.empty()) {
      const std::array<double, 2> at = station_to_world(road->plan_view, hover_->s, hover_->t);
      geometry.add_handle(at[0], at[1], kSignalOverlayLift, HandleKind::Node, HandleState::Hovered);
    }
  }

  if (!inspected_.is_valid()) {
    return geometry;
  }

  const std::vector<JunctionManeuverInfo> maneuvers = junction_maneuvers(network, inspected_);
  // Controller groups are shown STRUCTURALLY, as a chain linking the heads of
  // one group: PreviewGeometry carries no colour channel (it is two line
  // buffers plus handles), and adding one would mean renderer work this sprint
  // deliberately does not do. The chain is per-group and the leaders are
  // per-head, so a group is still unmistakable.
  std::map<std::string, std::vector<std::array<double, 3>>> groups;

  for (const JunctionApproachInfo& approach : approaches()) {
    for (const SignalId id : approach.signal_ids) {
      const std::optional<std::array<double, 3>> head = signal_world(network, id);
      if (!head.has_value()) {
        continue;
      }
      geometry.add_handle((*head)[0],
                          (*head)[1],
                          (*head)[2] + kSignalOverlayLift,
                          HandleKind::Sample,
                          HandleState::Idle);
      // Dotted leaders: this head to every movement it gates.
      for (const RoadId gated : approach.gated) {
        if (const std::optional<std::array<double, 3>> target =
                maneuver_midpoint(maneuvers, gated)) {
          append_segment(geometry.dashed_line_positions, *head, *target);
        }
      }
      for (const std::string& controller : approach.controller_odr_ids) {
        groups[controller].push_back(*head);
      }
    }
  }

  for (const auto& [controller, heads] : groups) {
    for (std::size_t i = 0; i + 1 < heads.size(); ++i) {
      append_segment(geometry.line_positions, heads[i], heads[i + 1]);
    }
  }
  return geometry;
}

QString SignalTool::instruction() const {
  if (!inspected_.is_valid()) {
    return tr("Click a junction to signalize it, or a road to place one signal");
  }
  return tr("The Attributes pane applies a template to this junction · click a road to place one "
            "signal · Esc clears the target");
}

} // namespace roadmaker::editor
