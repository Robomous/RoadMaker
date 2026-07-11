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

class PropertiesPanel : public QWidget {
  Q_OBJECT

public:
  PropertiesPanel(Document& document, const SelectionModel& selection, QWidget* parent = nullptr);

  /// The editor's road-mark width conventions [m]. OpenDRIVE's @width has no
  /// normative values (weight standard/bold is the spec's coarse axis) —
  /// these presets are RoadMaker conventions (docs/domain/opendrive.md).
  static constexpr double kMarkWidthStandard = 0.12;
  static constexpr double kMarkWidthBold = 0.25;

private:
  void refresh();
  void refresh_lane_section();
  void add_row(const QString& label, const QString& value);
  void clear_rows();

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
  QPushButton* remove_lane_;
};

} // namespace roadmaker::editor
