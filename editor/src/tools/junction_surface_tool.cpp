#include "tools/junction_surface_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// Height [m] the overlay floats above the floor it describes, so the rings and
/// sample dots read against the pavement instead of z-fighting it. Matched to
/// the junction detail lift the corner overlays use.
constexpr double kSpanOverlayLift = 0.02;

/// True when (x, y) is inside the closed ring `footprint` — the standard
/// crossing-number test, on the RAW pre-inflate ring the sort index arbitrates.
bool inside_ring(const std::vector<std::array<double, 2>>& footprint, double x, double y) {
  bool inside = false;
  const std::size_t count = footprint.size();
  for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
    const std::array<double, 2>& a = footprint[i];
    const std::array<double, 2>& b = footprint[j];
    if (((a[1] > y) != (b[1] > y)) && (x < (((b[0] - a[0]) * (y - a[1])) / (b[1] - a[1])) + a[0])) {
      inside = !inside;
    }
  }
  return inside;
}

void append_ring(std::vector<double>& lines, const std::vector<std::array<double, 2>>& footprint) {
  const std::size_t count = footprint.size();
  for (std::size_t i = 0; i < count; ++i) {
    const std::array<double, 2>& a = footprint[i];
    const std::array<double, 2>& b = footprint[(i + 1) % count];
    lines.insert(lines.end(), {a[0], a[1], kSpanOverlayLift, b[0], b[1], kSpanOverlayLift});
  }
}

} // namespace

JunctionSurfaceTool::JunctionSurfaceTool(Document& document,
                                         SelectionModel& selection,
                                         QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: every JunctionId the tool is holding
  // becomes stale (and can alias a fresh junction), so drop the lot.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
  // The tool is driven by the selection rather than by picking a junction of
  // its own — inspecting "the junction you already selected" is the whole
  // interaction.
  connect(&selection_, &SelectionModel::selection_changed, this, [this] { sync_to_selection(); });
}

void JunctionSurfaceTool::activate() {
  sync_to_selection();
}

void JunctionSurfaceTool::deactivate() {
  reset_all();
}

void JunctionSurfaceTool::reset_all() {
  const bool had_active = active_.has_value();
  inspected_ = JunctionId{};
  active_.reset();
  hovered_.reset();
  if (had_active) {
    emit surface_span_selection_changed();
  }
  emit preview_changed();
}

void JunctionSurfaceTool::sync_to_selection() {
  const JunctionId junction = selection_.primary().junction;
  // junction_surface_spans() is empty for a stale id AND for a junction with no
  // floor to control (a span/virtual junction has no connections at all), so it
  // is the whole eligibility test — no arms/spans special case here.
  const bool eligible =
      junction.is_valid() && !junction_surface_spans(document_.network(), junction).empty();
  const JunctionId next = eligible ? junction : JunctionId{};
  if (next == inspected_) {
    return;
  }
  inspected_ = next;
  const bool had_active = active_.has_value();
  active_.reset();
  hovered_.reset();
  if (had_active) {
    emit surface_span_selection_changed();
  }
  emit preview_changed();
}

std::vector<JunctionSurfaceSpanInfo> JunctionSurfaceTool::spans() const {
  return inspected_.is_valid() ? junction_surface_spans(document_.network(), inspected_)
                               : std::vector<JunctionSurfaceSpanInfo>{};
}

std::optional<JunctionSurfaceSpanInfo> JunctionSurfaceTool::active_span_info() const {
  if (!active_.has_value()) {
    return std::nullopt;
  }
  for (const JunctionSurfaceSpanInfo& info : spans()) {
    if (info.road == active_->road) {
      return info;
    }
  }
  return std::nullopt;
}

std::optional<RoadId> JunctionSurfaceTool::span_under(double world_x, double world_y) const {
  std::optional<RoadId> best;
  int best_sort = 0;
  for (const JunctionSurfaceSpanInfo& info : spans()) {
    if (info.footprint.size() < 3 || !inside_ring(info.footprint, world_x, world_y)) {
      continue;
    }
    // Highest sort wins, ties to the LATER span in connection order — the one
    // drawn on top is the one that answers the click.
    if (!best.has_value() || info.sort_index >= best_sort) {
      best = info.road;
      best_sort = info.sort_index;
    }
  }
  return best;
}

void JunctionSurfaceTool::set_active(std::optional<ActiveSurfaceSpan> span) {
  const bool changed = span != active_;
  active_ = std::move(span);
  if (active_.has_value()) {
    // The rest of the UI follows the junction (there is no span-level selection
    // entry); Replace so a click reads like every other click.
    selection_.select(SelectionEntry{.junction = active_->junction}, SelectMode::Replace);
  }
  if (changed) {
    emit surface_span_selection_changed();
  }
  emit preview_changed();
}

void JunctionSurfaceTool::select_span(RoadId road) {
  if (!inspected_.is_valid()) {
    return;
  }
  const std::vector<JunctionSurfaceSpanInfo> all = spans();
  const bool known = std::ranges::any_of(
      all, [&](const JunctionSurfaceSpanInfo& info) { return info.road == road; });
  set_active(known ? std::optional<ActiveSurfaceSpan>(
                         ActiveSurfaceSpan{.junction = inspected_, .road = road})
                   : std::nullopt);
}

bool JunctionSurfaceTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // A junction floor under the cursor makes it the inspected junction even
  // before the selection catches up, so one click both selects and inspects.
  if (event.pick.has_value() && event.pick->junction.is_valid() &&
      event.pick->junction != inspected_) {
    selection_.select(SelectionEntry{.junction = event.pick->junction}, SelectMode::Replace);
    sync_to_selection();
  }
  if (!inspected_.is_valid()) {
    return false; // nothing to inspect — let the viewport keep the click
  }
  if (const std::optional<RoadId> road = span_under(event.world_x, event.world_y)) {
    set_active(ActiveSurfaceSpan{.junction = inspected_, .road = *road});
    emit status_message(
        tr("Space toggles this span's samples · PgUp/PgDn raise or lower it · Tab cycles"));
    return true;
  }
  set_active(std::nullopt);
  return false;
}

bool JunctionSurfaceTool::mouse_move(const ToolEvent& event) {
  std::optional<RoadId> hovered = span_under(event.world_x, event.world_y);
  if (hovered != hovered_) {
    hovered_ = hovered;
    emit preview_changed();
  }
  return false; // hovering never consumes: camera nav and the readout stay live
}

bool JunctionSurfaceTool::toggle_active_included() {
  const std::optional<JunctionSurfaceSpanInfo> info = active_span_info();
  if (!info.has_value()) {
    return false;
  }
  if (!document_
           .push_command(edit::set_surface_span_included(
               document_.network(), active_->junction, active_->road, !info->included))
           .has_value()) {
    emit toast_requested(tr("That span's samples could not be changed"), ToastSeverity::Warning);
    return true;
  }
  emit status_message(info->included ? tr("Span samples excluded — Ctrl+Z to undo")
                                     : tr("Span samples included — Ctrl+Z to undo"));
  emit surface_span_selection_changed();
  emit preview_changed();
  return true;
}

bool JunctionSurfaceTool::nudge_active_sort_index(int delta) {
  const std::optional<JunctionSurfaceSpanInfo> info = active_span_info();
  if (!info.has_value() || delta == 0) {
    return false;
  }
  // Raise/Lower is editor-side `current +/- 1` on purpose: a single-record
  // command needs no renumbering pass, so a sort index survives regeneration.
  if (!document_
           .push_command(edit::set_surface_span_sort_index(
               document_.network(), active_->junction, active_->road, info->sort_index + delta))
           .has_value()) {
    emit toast_requested(tr("That span is already as far as it goes"), ToastSeverity::Warning);
    return true;
  }
  emit status_message(delta > 0 ? tr("Span raised — Ctrl+Z to undo")
                                : tr("Span lowered — Ctrl+Z to undo"));
  emit surface_span_selection_changed();
  emit preview_changed();
  return true;
}

bool JunctionSurfaceTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Space) {
    return toggle_active_included();
  }
  if (key == Qt::Key_PageUp) {
    return nudge_active_sort_index(+1);
  }
  if (key == Qt::Key_PageDown) {
    return nudge_active_sort_index(-1);
  }
  if (key == Qt::Key_Tab) {
    const std::vector<JunctionSurfaceSpanInfo> all = spans();
    if (all.empty()) {
      return false;
    }
    std::size_t next = 0;
    if (active_.has_value()) {
      for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].road == active_->road) {
          next = (i + 1) % all.size();
          break;
        }
      }
    }
    set_active(ActiveSurfaceSpan{.junction = inspected_, .road = all[next].road});
    return true;
  }
  if (key == Qt::Key_Escape && active_.has_value()) {
    set_active(std::nullopt);
    return true;
  }
  return false;
}

PreviewGeometry JunctionSurfaceTool::preview() const {
  PreviewGeometry geometry;
  for (const JunctionSurfaceSpanInfo& info : spans()) {
    const bool active = active_.has_value() && active_->road == info.road;
    const bool lit = active || (hovered_.has_value() && *hovered_ == info.road);
    // Excluded spans are DASHED: their footprint is still in the union (the
    // pavement does not move), but their samples no longer shape it.
    append_ring(info.included ? geometry.line_positions : geometry.dashed_line_positions,
                info.footprint);
    // Samples only for the span under attention — every span's dots at once is
    // an unreadable cloud on a twelve-turn crossing.
    if (!lit) {
      continue;
    }
    const HandleState state = active ? HandleState::Hovered : HandleState::Idle;
    for (const std::array<double, 3>& sample : info.border) {
      geometry.add_handle(
          sample[0], sample[1], sample[2] + kSpanOverlayLift, HandleKind::Sample, state);
    }
    for (const std::array<double, 3>& sample : info.centerline) {
      geometry.add_handle(
          sample[0], sample[1], sample[2] + kSpanOverlayLift, HandleKind::Sample, state);
    }
  }
  return geometry;
}

QString JunctionSurfaceTool::instruction() const {
  if (!inspected_.is_valid()) {
    return tr("Select a junction to inspect its surface spans");
  }
  return tr("Click a span to inspect it · Space toggles its samples · PgUp/PgDn raise or lower it "
            "· Tab cycles");
}

} // namespace roadmaker::editor
