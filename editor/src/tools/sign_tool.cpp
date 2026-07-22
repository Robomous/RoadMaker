#include "tools/sign_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal.hpp"

#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/signal_placement.hpp"

namespace roadmaker::editor {

namespace {

/// Height [m] the placement ghost floats above the pavement.
constexpr double kSignOverlayLift = 0.05;

/// Cursor travel [m] past which an armed placement becomes a drag.
constexpr double kDragTolerance = 0.5;

} // namespace

SignTool::SignTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: drop any armed placement / hover ghost.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
}

void SignTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

QString SignTool::current_tag() const {
  const LibraryItem item = params_provider_ ? params_provider_() : LibraryItem{};
  // A selected sign asset places that sign; anything else defaults to the text
  // plate so the tool is usable with no Library interaction.
  if (is_signal_asset(item) && !item.signal.isEmpty()) {
    return item.signal;
  }
  return QStringLiteral("sign_text");
}

void SignTool::activate() {}

void SignTool::deactivate() {
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  reset_all();
}

void SignTool::reset_all() {
  press_.reset();
  hover_.reset();
  emit preview_changed();
}

bool SignTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || press_.has_value()) {
    return false;
  }
  // A sign under the cursor is selected, not replaced — its face text is then
  // editable in the Attributes pane.
  if (event.pick.has_value() && event.pick->signal.is_valid()) {
    selection_.select(SelectionEntry{.road = event.pick->road, .signal = event.pick->signal},
                      SelectMode::Replace);
    emit status_message(tr("Sign selected — edit its text in the Attributes pane"));
    emit preview_changed();
    return true;
  }
  // On or beside a road, arm a placement resolved on release. A press in open
  // space arms nothing (OpenDRIVE has no world-placed signal).
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

bool SignTool::mouse_move(const ToolEvent& event) {
  if (press_.has_value()) {
    const bool beyond = std::abs(event.world_x - press_->world_x) > kDragTolerance ||
                        std::abs(event.world_y - press_->world_y) > kDragTolerance;
    if (beyond && press_->placement.has_value()) {
      update_drag(event);
    }
    return true;
  }
  // Plain hover: ghost where a click would land. Never consumes.
  hover_ = nearest_signal_station(document_.network(), event.world_x, event.world_y);
  emit preview_changed();
  return false;
}

void SignTool::update_drag(const ToolEvent& event) {
  const std::optional<RoadStation> station =
      nearest_signal_station(document_.network(), event.world_x, event.world_y);
  if (!station.has_value()) {
    return; // off every road: hold the last good frame
  }
  press_->placement = station;
  press_->dragging = true;
  const QString tag = current_tag();
  // The factory runs against the BASE network each frame, so the same add_signal
  // is simply replaced — one command, one undo entry on release.
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

void SignTool::place_sign(const RoadStation& placement) {
  Signal signal =
      make_signal(current_tag(), next_signal_odr_id(document_.network()), placement.s, placement.t);
  const std::string odr = signal.odr_id;
  const RoadId road = placement.road;
  if (!document_.push_command(edit::add_signal(document_.network(), road, std::move(signal)))
           .has_value()) {
    emit status_message(tr("Couldn't place a sign here"));
    return;
  }
  // add_signal mints the SignalId only on apply, so look the head up by its odr
  // id on the owning road and select it (its text is editable in Attributes).
  document_.network().for_each_signal([&](SignalId id, const Signal& placed) {
    if (placed.road == road && placed.odr_id == odr) {
      selection_.select(SelectionEntry{.road = road, .signal = id}, SelectMode::Replace);
    }
  });
  hover_.reset();
  emit status_message(tr("Sign placed — edit its text in the Attributes pane"));
  emit preview_changed();
}

bool SignTool::mouse_release(const ToolEvent& event) {
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
      emit status_message(tr("Sign placed — edit its text in the Attributes pane"));
    }
    emit preview_changed();
    return true;
  }
  static_cast<void>(event);
  if (state.placement.has_value()) {
    place_sign(*state.placement);
    return true;
  }
  return false;
}

bool SignTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape || !press_.has_value()) {
    return false;
  }
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  press_.reset();
  emit status_message(tr("Sign placement cancelled"));
  emit preview_changed();
  return true;
}

PreviewGeometry SignTool::preview() const {
  PreviewGeometry geometry;
  // The ghost: where a click would place a sign. Suppressed during a drag — the
  // live preview session already shows the real sign.
  if (!press_.has_value() && hover_.has_value()) {
    const RoadNetwork& network = document_.network();
    if (const Road* road = network.road(hover_->road);
        road != nullptr && !road->plan_view.empty()) {
      const std::array<double, 2> at = station_to_world(road->plan_view, hover_->s, hover_->t);
      geometry.add_handle(at[0], at[1], kSignOverlayLift, HandleKind::Node, HandleState::Hovered);
    }
  }
  return geometry;
}

QString SignTool::instruction() const {
  return tr("Click a road to place a sign, or drag to slide it along. Its face text is "
            "editable in the Attributes pane.");
}

} // namespace roadmaker::editor
