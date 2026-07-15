#pragma once

// Properties of the selected entity: read-only rows rebuilt from the kernel
// on every selection change, plus manually bound editors (issues #14/#15,
// docs/design/m2/01_editing_framework.md §7) — the road name and the Lane
// Profile section edit the primary selection through Document commands, one
// command per discrete panel action. Text/spin editors commit on
// editingFinished only and skip the push when the value did not change, so
// refresh-on-undo never echoes a command back.

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/scrub_label.hpp"
#include "panels/slot_widget.hpp"

namespace roadmaker::editor {

class ElevationTool;

class PropertiesPanel : public QWidget {
  Q_OBJECT

public:
  PropertiesPanel(Document& document, const SelectionModel& selection, QWidget* parent = nullptr);

signals:
  /// A discrete panel action produced a user-facing result (e.g. a lane was
  /// removed) — MainWindow routes it to the viewport toast overlay. The panel
  /// never touches the viewport itself.
  void status_message(const QString& text);

  /// A slot was engaged: MainWindow should show `category` in the Library. The
  /// panel does not know the Library exists.
  void library_category_requested(const QString& category);

public:
  /// Wires the Elevation editing section to the Elevation tool's active node
  /// (issue #16). Until a tool is attached the section stays hidden; the
  /// panel never owns the tool. Call once, after both exist.
  void set_elevation_tool(ElevationTool* tool);

  /// The editor's road-mark width conventions [m]. OpenDRIVE's @width has no
  /// normative values (weight standard/bold is the spec's coarse axis) —
  /// these presets are RoadMaker conventions (docs/domain/opendrive.md).
  static constexpr double kMarkWidthStandard = 0.12;
  static constexpr double kMarkWidthBold = 0.25;

private:
  void refresh();
  void refresh_lane_section();
  void refresh_elevation();
  void add_row(const QString& label, const QString& value);
  void clear_rows();

  // --- scrub-editing (P1/GW-2) ------------------------------------------------
  // One numeric attribute a ScrubLabel can drag. The table is deliberately
  // LOCAL to this panel: it is the only consumer, and a shared descriptor
  // registry would be speculative until a second one appears.

  struct ScrubBinding {
    QDoubleSpinBox* spin = nullptr; ///< live readout; also supplies the range clamp
    double units_per_pixel = 0.0;

    /// The attribute's current value, or nullopt when it is not editable right
    /// now (nothing selected, no active node) — the gesture then does nothing.
    std::function<std::optional<double>()> baseline;

    /// The command that sets the attribute to `value`, built against the
    /// preview session's BASE-state network (Document::PreviewFactory).
    std::function<std::unique_ptr<edit::Command>(const RoadNetwork&, double)> factory;
  };

  /// Registers `binding` against `label` and wires the gesture to one preview
  /// session. Returns the label so it can be dropped straight into a form row.
  ScrubLabel* install_scrub(ScrubLabel* label, ScrubBinding binding);

  void begin_scrub(std::size_t index);
  void update_scrub(double delta);
  void finish_scrub();
  void cancel_scrub();

  std::vector<ScrubBinding> scrubs_;

  /// The in-flight gesture's binding, baseline, and latest value. Empty index
  /// = no scrub (and no preview session owned by this panel).
  std::optional<std::size_t> scrub_active_;
  double scrub_baseline_ = 0.0;
  double scrub_value_ = 0.0;

  /// Commits the dropped library item into the primary prop's model slot.
  void push_object_model(const QString& key);

  /// Height of the Elevation tool's active node, or nullopt when no tool, no
  /// active node, or a stale road/index. Shared by the Height spin box and its
  /// scrub binding so both agree on what "current" means.
  [[nodiscard]] std::optional<double> active_node_height() const;

  /// Removes the outermost lane on `side` (>0 left, <0 right) of the target
  /// section — no lane selection needed. Emits status_message on success.
  void remove_outermost_lane(int side);

  /// Pushes one command; a kernel refusal surfaces through the document's
  /// diagnostics (push_command appends one), never a crash or a stale panel
  /// — refresh() re-syncs from the network afterwards either way.
  void push(std::unique_ptr<edit::Command> command);

  /// The primary selection's lane (invalid id when road-level or empty).
  [[nodiscard]] const Lane* primary_lane() const;

  /// The section lane edits act on: the primary lane's, or the road's first.
  [[nodiscard]] LaneSectionId target_section() const;

  Document& document_;
  const SelectionModel& selection_;
  QFormLayout* form_;
  QLabel* placeholder_;

  QWidget* name_row_;
  QLineEdit* name_edit_;

  QGroupBox* lane_group_;
  QComboBox* type_combo_;
  QDoubleSpinBox* width_spin_;
  QComboBox* mark_combo_;
  QDoubleSpinBox* mark_width_spin_;
  QPushButton* add_left_;
  QPushButton* add_right_;
  QPushButton* remove_left_;
  QPushButton* remove_right_;

  QGroupBox* elevation_group_;
  QLabel* elevation_node_label_;
  QDoubleSpinBox* elevation_spin_;
  ElevationTool* elevation_tool_ = nullptr;

  QGroupBox* signal_group_;
  QDoubleSpinBox* signal_s_spin_;
  QDoubleSpinBox* signal_t_spin_;
  QDoubleSpinBox* signal_h_spin_;
  QLabel* signal_kind_label_;

  /// Prop section: a selected object used to show its owning ROAD's fields.
  /// The Model slot is the first slot consumer (GW-3 mechanics).
  QGroupBox* object_group_;
  QLabel* object_kind_label_;
  SlotWidget* model_slot_;

  /// Populates the Prop section from the primary selection's object, and shows
  /// it.
  void refresh_object(const Object& object);

  /// Populates the Signal section (position spinboxes + read-only type rows)
  /// from the primary selection's signal, and shows it.
  void refresh_signal(const Signal& signal);

  /// Commits a move_signal from the spinbox values for the primary signal.
  void push_signal_move();
};

} // namespace roadmaker::editor
