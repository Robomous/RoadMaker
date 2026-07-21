#include "tools/crosswalk_stop_line_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <QUndoStack>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "document/crosswalk_placement.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// The chevron half-arm length [m] and opening angle — a ">" pointing into the
/// junction, sized to read at typical intersection zoom.
constexpr double kChevronArm = 2.0;
constexpr double kChevronSpread = 0.6; // [rad] half-angle of the opening

} // namespace

CrosswalkStopLineTool::CrosswalkStopLineTool(Document& document,
                                             SelectionModel& selection,
                                             QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void CrosswalkStopLineTool::set_params_provider(std::function<edit::CrosswalkParams()> provider) {
  params_provider_ = std::move(provider);
}

void CrosswalkStopLineTool::deactivate() {
  reset();
}

void CrosswalkStopLineTool::reset() {
  hover_.reset();
  emit preview_changed();
}

bool CrosswalkStopLineTool::mouse_press(const ToolEvent& event) {
  // LMB belongs to the tool (M2 button map) even on a miss, so a click over an
  // approach never leaks to camera navigation; placement happens on release.
  return static_cast<bool>(event.buttons & Qt::LeftButton);
}

bool CrosswalkStopLineTool::mouse_move(const ToolEvent& event) {
  hover_ = nearest_junction_arm(
      document_.network(), event.world_x, event.world_y, kCrosswalkSnapThreshold);
  emit preview_changed();
  return false; // hover never consumes: camera/other handlers still see it
}

bool CrosswalkStopLineTool::mouse_release(const ToolEvent& event) {
  const std::optional<ArmHit> arm = nearest_junction_arm(
      document_.network(), event.world_x, event.world_y, kCrosswalkSnapThreshold);
  if (!arm.has_value()) {
    emit toast_requested(tr("Click a junction approach to place a crosswalk + stop line"),
                         ToastSeverity::Info);
    return true;
  }
  const edit::CrosswalkParams params =
      params_provider_ ? params_provider_() : edit::CrosswalkParams{};
  std::optional<std::pair<RoadId, Object>> placed =
      crosswalk_for_arm(document_.network(), arm->junction, arm->arm_road, params);
  if (!placed.has_value()) {
    emit toast_requested(tr("That approach has no lanes to cross"), ToastSeverity::Warning);
    return true;
  }

  // The crosswalk carries a known odr_id (assigned by the generator); remember
  // it so the placed instance can be selected once the add applies, and so the
  // stop-line record can point back at it.
  const RoadId crosswalk_road = placed->first;
  const std::string crosswalk_odr = placed->second.odr_id;

  // ONE undo unit: the crosswalk object and the stop-line link land together
  // (context_menu's macro pattern), so a single Ctrl+Z removes the whole
  // placement (GW-5). The stop line itself already exists — it is derived — so
  // the second command only records the setback and the provenance link.
  document_.undo_stack()->beginMacro(tr("Place crosswalk"));
  (void)document_.push_command(
      edit::add_object(document_.network(), crosswalk_road, std::move(placed->second)));
  const RoadEnd stopline_arm{.road = arm->arm_road, .contact = arm->contact};
  (void)document_.push_command(edit::set_stopline_distance(
      document_.network(), arm->junction, stopline_arm, kStopLineDefaultDistance, crosswalk_odr));
  document_.undo_stack()->endMacro();

  // Select the placed crosswalk (looked up by its odr_id on the arm road, since
  // add_object mints the ObjectId only on apply).
  if (!crosswalk_odr.empty()) {
    document_.network().for_each_object([&](ObjectId id, const Object& object) {
      if (object.road == crosswalk_road && object.odr_id == crosswalk_odr) {
        selection_.select({.road = crosswalk_road, .object = id}, SelectMode::Replace);
      }
    });
  }
  emit status_message(tr("Placed crosswalk + stop line — Ctrl+Z to undo"));
  return true;
}

PreviewGeometry CrosswalkStopLineTool::preview() const {
  PreviewGeometry geometry;
  if (!hover_.has_value()) {
    return geometry;
  }
  // A chevron ">" whose apex sits ahead of the arm end toward the junction and
  // whose two arms open back away from it — the placement affordance (GW-2 s10).
  const double h = hover_->heading;
  const double apex_x = hover_->anchor_x + (std::cos(h) * kChevronArm);
  const double apex_y = hover_->anchor_y + (std::sin(h) * kChevronArm);
  const auto add_segment = [&](double ax, double ay, double bx, double by) {
    geometry.line_positions.insert(geometry.line_positions.end(), {ax, ay, 0.0, bx, by, 0.0});
  };
  for (const double sign : {-1.0, 1.0}) {
    const double tail = h + std::numbers::pi + (sign * kChevronSpread);
    add_segment(apex_x,
                apex_y,
                apex_x + (std::cos(tail) * kChevronArm),
                apex_y + (std::sin(tail) * kChevronArm));
  }
  geometry.add_handle(
      hover_->anchor_x, hover_->anchor_y, 0.0, HandleKind::Node, HandleState::Hovered);
  return geometry;
}

QString CrosswalkStopLineTool::instruction() const {
  return tr("Click a junction approach to place a crosswalk + stop line");
}

} // namespace roadmaker::editor
