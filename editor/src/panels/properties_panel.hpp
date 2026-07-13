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

#include "document/document.hpp"
#include "document/selection_model.hpp"

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
};

} // namespace roadmaker::editor
