#pragma once

// Signal tool (p4-s7, issue #228). The authoring surface for the signalization
// layer: it makes a junction the tool's target so the Attributes pane can offer
// the auto-signalize templates, selects an already-placed head, and places a
// single signal from the Library on a road.
//
// It owns the PENDING template and mount model (the state the panel's combos
// edit) rather than the panel, for the same reason every other tool owns its
// sub-selection: the decision is editing logic, it must be testable headless,
// and the panel is a thin view over it. What is APPLIED lives on the junction
// (Junction::signalization) and is read back from the live network, never
// cached.
//
// Gating is derived, not stored: `junction_signals()` is the single query the
// tool, the panel, the commands and Python all read, so nothing here re-derives
// which movements a head controls.
//
// Placement follows the M2 drag rule: a plain click places one signal as ONE
// add_signal command; a drag opens a preview session and commits exactly ONE
// command on release (no mergeWith). Esc cancels; deactivate() cancels.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/id.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp" // RoadStation
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// The `rm:signal` token a template is persisted as (road/junction.hpp's
/// `kSignalizationTemplates`). Free function so the panel and the context menu
/// can compare a pending template against `Junction::signalization` without
/// instantiating a tool.
[[nodiscard]] std::string_view signalize_template_token(edit::SignalizeTemplate tmpl);

/// The template `token` names, or nullopt when it names none (an unsignalized
/// junction, or a token from a newer build).
[[nodiscard]] std::optional<edit::SignalizeTemplate>
signalize_template_from_token(std::string_view token);

class SignalTool : public Tool {
  Q_OBJECT

public:
  SignalTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The signal asset a click on a road places. MainWindow wires this to the
  /// Library's selected item; an incompatible/unset item makes a click toast
  /// rather than place.
  void set_params_provider(std::function<LibraryItem()> provider);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Every approach of the target junction drawn as a leader from its head(s)
  /// to the maneuvers they gate, plus one chain per controller group.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The junction whose signalization is on show, or an invalid id.
  [[nodiscard]] JunctionId inspected_junction() const { return inspected_; }

  /// The target junction's approaches, solved against the CURRENT network.
  [[nodiscard]] std::vector<JunctionApproachInfo> approaches() const;

  /// The template the next Auto Signalize applies (the panel's combo).
  [[nodiscard]] edit::SignalizeTemplate pending_template() const { return pending_.tmpl; }

  void set_pending_template(edit::SignalizeTemplate tmpl);

  /// The prop model mounted with each placed head; empty = none (the #323
  /// extension point).
  [[nodiscard]] const std::string& pending_mount_model() const { return pending_.mount_model; }

  void set_pending_mount_model(std::string model);

  /// The template ALREADY applied to the target junction, read off the live
  /// network. nullopt when nothing is applied (or the token is unknown).
  [[nodiscard]] std::optional<edit::SignalizeTemplate> applied_template() const;

  /// Whether `signalize()` / `clear()` can run right now. False for a stale or
  /// unset target, a FOREIGN junction (no arms to place against), a SPAN
  /// junction (ASAM: virtual junctions have no controllers), and — for
  /// signalize — a pending template+mount that already equals what is applied,
  /// which the factory rejects as a no-op. The UI DISABLES on these rather than
  /// letting a command fail.
  [[nodiscard]] bool can_signalize() const;
  [[nodiscard]] bool can_clear() const;

  /// Why signalize is unavailable, for a tooltip. Empty when it is available.
  [[nodiscard]] QString signalize_blocker() const;

  /// Pushes signalize_junction / clear_signalization on the target junction.
  /// False when the precondition fails or the kernel refused (a toast is
  /// emitted in that case).
  bool signalize();
  bool clear();

signals:
  /// The target junction, the pending template/mount, or the applied state
  /// changed. Carries no payload — listeners pull the getters above.
  void signalization_changed();

private:
  /// An armed placement: where the press landed and the station it snapped to.
  /// The station is the PRESS-TIME snapshot; a drag recomputes from the cursor
  /// against the same road, so a drag never hops to a neighbouring road.
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    std::optional<RoadStation> placement;
    bool dragging = false;
  };

  [[nodiscard]] LibraryItem current_item() const;

  /// Re-reads the selection: a junction selection (or a connecting road's
  /// junction) becomes the target, anything else leaves the target alone —
  /// unlike the Maneuver tool, a selected head must not clear the junction
  /// whose panel rows the author is reading.
  void sync_to_selection();

  void set_inspected(JunctionId junction);

  /// The junction the event points at: a picked floor or a picked connecting
  /// road names one outright, otherwise the one already targeted.
  [[nodiscard]] JunctionId resolve_junction(const ToolEvent& event) const;

  void place_signal(const RoadStation& placement);
  void update_drag(const ToolEvent& event);
  void reset_all();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;

  JunctionId inspected_;
  edit::SignalizeOptions pending_;
  std::optional<PressState> press_;
  /// The ghost pose while hovering (not dragging).
  std::optional<RoadStation> hover_;
  /// Guards the selection round trip: selecting the target junction re-enters
  /// sync_to_selection.
  bool syncing_ = false;
};

} // namespace roadmaker::editor
