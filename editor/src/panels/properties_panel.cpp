#include "panels/properties_panel.hpp"

#include "roadmaker/edit/operations.hpp"

#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <utility>

namespace roadmaker::editor {

namespace {

/// Panel display names for the LaneType subset the M2 editor offers (spec 02
/// §4); the selected lane's current type joins the list when it is not one
/// of them, so the combo always shows the truth.
constexpr std::array<std::pair<LaneType, const char*>, 5> kTypeChoices{{
    {LaneType::Driving, "Driving"},
    {LaneType::Shoulder, "Shoulder"},
    {LaneType::Sidewalk, "Sidewalk"},
    {LaneType::Median, "Median"},
    {LaneType::Border, "Border"},
}};

constexpr std::array<std::pair<RoadMarkType, const char*>, 3> kMarkChoices{{
    {RoadMarkType::Solid, "Solid"},
    {RoadMarkType::Broken, "Broken"},
    {RoadMarkType::None, "None"},
}};

QString lane_type_name(LaneType type) {
  for (const auto& [value, name] : kTypeChoices) {
    if (value == type) {
      return QString::fromLatin1(name);
    }
  }
  switch (type) {
  case LaneType::Stop:
    return QStringLiteral("Stop");
  case LaneType::Biking:
    return QStringLiteral("Biking");
  case LaneType::Restricted:
    return QStringLiteral("Restricted");
  case LaneType::Parking:
    return QStringLiteral("Parking");
  case LaneType::Curb:
    return QStringLiteral("Curb");
  case LaneType::None:
    return QStringLiteral("None");
  default:
    return QStringLiteral("Other");
  }
}

QString mark_type_name(RoadMarkType type) {
  for (const auto& [value, name] : kMarkChoices) {
    if (value == type) {
      return QString::fromLatin1(name);
    }
  }
  switch (type) {
  case RoadMarkType::SolidSolid:
    return QStringLiteral("Solid solid");
  case RoadMarkType::SolidBroken:
    return QStringLiteral("Solid broken");
  case RoadMarkType::BrokenSolid:
    return QStringLiteral("Broken solid");
  default:
    return QStringLiteral("Other");
  }
}

/// Rebuilds `combo` with the fixed choice list plus, when absent, `current`
/// (as a display-only extra), and selects `current`.
template <typename Enum, std::size_t N>
void rebuild_choice_combo(QComboBox& combo,
                          const std::array<std::pair<Enum, const char*>, N>& choices,
                          Enum current,
                          QString (*name_of)(Enum)) {
  const QSignalBlocker blocker(&combo);
  combo.clear();
  for (const auto& [value, name] : choices) {
    combo.addItem(QString::fromLatin1(name), static_cast<int>(value));
  }
  if (combo.findData(static_cast<int>(current)) < 0) {
    combo.addItem(name_of(current), static_cast<int>(current));
  }
  combo.setCurrentIndex(combo.findData(static_cast<int>(current)));
}

/// No other lane of the section sits further out on the lane's side — the
/// only removable position in M2 (keeps OpenDRIVE numbering contiguous).
bool is_outermost(const RoadNetwork& network, const Lane& lane) {
  const LaneSection* section = network.lane_section(lane.section);
  if (section == nullptr) {
    return false;
  }
  for (const LaneId other_id : section->lanes) {
    const int other = network.lane(other_id)->odr_id;
    if ((lane.odr_id > 0 && other > lane.odr_id) || (lane.odr_id < 0 && other < lane.odr_id)) {
      return false;
    }
  }
  return true;
}

} // namespace

PropertiesPanel::PropertiesPanel(Document& document,
                                 const SelectionModel& selection,
                                 QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection), form_(new QFormLayout),
      placeholder_(new QLabel(tr("Select a road or lane."), this)),
      lane_group_(new QGroupBox(tr("Lane profile"), this)), type_combo_(new QComboBox),
      width_spin_(new QDoubleSpinBox), mark_combo_(new QComboBox),
      mark_width_spin_(new QDoubleSpinBox), add_left_(new QPushButton(tr("Add left"))),
      add_right_(new QPushButton(tr("Add right"))),
      remove_lane_(new QPushButton(tr("Remove lane"))) {
  placeholder_->setWordWrap(true);
  placeholder_->setEnabled(false);

  type_combo_->setObjectName(QStringLiteral("lane_type_combo"));
  width_spin_->setObjectName(QStringLiteral("lane_width_spin"));
  width_spin_->setRange(0.01, 50.0);
  width_spin_->setSingleStep(0.25);
  width_spin_->setDecimals(2);
  width_spin_->setSuffix(tr(" m"));
  mark_combo_->setObjectName(QStringLiteral("road_mark_combo"));
  mark_width_spin_->setObjectName(QStringLiteral("road_mark_width_spin"));
  mark_width_spin_->setRange(0.0, 2.0);
  mark_width_spin_->setSingleStep(0.01);
  mark_width_spin_->setDecimals(2);
  mark_width_spin_->setSuffix(tr(" m"));
  mark_width_spin_->setValue(kMarkWidthStandard);
  mark_width_spin_->setToolTip(
      tr("Painted line width. Conventions: %1 m standard, %2 m bold (the OpenDRIVE spec sets "
         "no numeric values).")
          .arg(kMarkWidthStandard)
          .arg(kMarkWidthBold));
  add_left_->setObjectName(QStringLiteral("add_left_lane_button"));
  add_right_->setObjectName(QStringLiteral("add_right_lane_button"));
  remove_lane_->setObjectName(QStringLiteral("remove_lane_button"));

  auto* lane_form = new QFormLayout;
  lane_form->addRow(tr("Type"), type_combo_);
  lane_form->addRow(tr("Width"), width_spin_);
  lane_form->addRow(tr("Road mark"), mark_combo_);
  lane_form->addRow(tr("Mark width"), mark_width_spin_);
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(add_left_);
  buttons->addWidget(add_right_);
  buttons->addWidget(remove_lane_);
  auto* group_layout = new QVBoxLayout(lane_group_);
  group_layout->addLayout(lane_form);
  group_layout->addLayout(buttons);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(placeholder_);
  layout->addLayout(form_);
  layout->addWidget(lane_group_);
  layout->addStretch();

  // One command per discrete action (spec 02 §4). Combos commit on
  // `activated` — user gestures only, never programmatic refresh; spins on
  // editingFinished, skipping pushes when the value did not change.
  connect(type_combo_, &QComboBox::activated, this, [this](int index) {
    if (primary_lane() != nullptr) {
      push(edit::set_lane_type(document_.network(),
                               selection_.primary().lane,
                               static_cast<LaneType>(type_combo_->itemData(index).toInt())));
    }
  });
  connect(width_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    const Lane* lane = primary_lane();
    if (lane == nullptr || lane->widths.empty()) {
      return;
    }
    if (std::abs(width_spin_->value() - lane->widths.front().a) < 1e-9) {
      return;
    }
    push(
        edit::set_lane_width(document_.network(), selection_.primary().lane, width_spin_->value()));
  });
  const auto push_mark = [this] {
    const Lane* lane = primary_lane();
    if (lane == nullptr) {
      return;
    }
    RoadMark mark = lane->road_marks.empty() ? RoadMark{} : lane->road_marks.front();
    mark.type = static_cast<RoadMarkType>(mark_combo_->currentData().toInt());
    mark.width = mark_width_spin_->value();
    push(edit::set_road_mark(document_.network(), selection_.primary().lane, mark));
  };
  connect(mark_combo_, &QComboBox::activated, this, push_mark);
  connect(mark_width_spin_, &QDoubleSpinBox::editingFinished, this, [this, push_mark] {
    const Lane* lane = primary_lane();
    if (lane == nullptr) {
      return;
    }
    const double current =
        lane->road_marks.empty() ? kMarkWidthStandard : lane->road_marks.front().width;
    if (std::abs(mark_width_spin_->value() - current) < 1e-9) {
      return;
    }
    push_mark();
  });
  connect(add_left_, &QPushButton::clicked, this, [this] {
    push(edit::add_lane(document_.network(), target_section(), +1, LaneType::Driving));
  });
  connect(add_right_, &QPushButton::clicked, this, [this] {
    push(edit::add_lane(document_.network(), target_section(), -1, LaneType::Driving));
  });
  connect(remove_lane_, &QPushButton::clicked, this, [this] {
    if (primary_lane() != nullptr) {
      push(edit::remove_lane(document_.network(), selection_.primary().lane));
    }
  });

  connect(&selection_, &SelectionModel::selection_changed, this, &PropertiesPanel::refresh);
  connect(&document_, &Document::loaded, this, &PropertiesPanel::refresh);
  // Commands and undo/redo change lane values without touching the
  // selection — re-sync the editors from the network.
  connect(&document_, &Document::mesh_changed, this, &PropertiesPanel::refresh);
  refresh();
}

void PropertiesPanel::refresh() {
  clear_rows();

  // The primary entry (most recently selected) drives the panel.
  const SelectionEntry primary = selection_.primary();
  const Road* road = document_.network().road(primary.road);
  if (road == nullptr) {
    placeholder_->show();
    lane_group_->hide();
    return;
  }
  placeholder_->hide();

  if (selection_.entries().size() > 1) {
    add_row(tr("Selection"), tr("%1 items").arg(selection_.entries().size()));
  }
  add_row(tr("Road"),
          road->name.empty() ? QString::fromStdString(road->odr_id)
                             : QString::fromStdString(road->name));
  add_row(tr("OpenDRIVE id"), QString::fromStdString(road->odr_id));
  add_row(tr("Length"), tr("%1 m").arg(road->length, 0, 'f', 3));
  add_row(tr("Geometry records"), QString::number(road->plan_view.records().size()));
  add_row(tr("Lane sections"), QString::number(road->sections.size()));

  const Lane* lane = document_.network().lane(primary.lane);
  if (lane != nullptr) {
    add_row(tr("Lane"), QString::number(lane->odr_id));
    add_row(tr("Width records"), QString::number(lane->widths.size()));
    add_row(tr("Road marks"), QString::number(lane->road_marks.size()));
  }

  lane_group_->show();
  refresh_lane_section();
}

void PropertiesPanel::refresh_lane_section() {
  const Lane* lane = primary_lane();
  const bool lane_selected = lane != nullptr;
  const bool center = lane_selected && lane->odr_id == 0;

  // Add-lane buttons act on the section, so a road-level selection suffices.
  add_left_->setEnabled(true);
  add_right_->setEnabled(true);

  type_combo_->setEnabled(lane_selected && !center);
  // The center lane is width-less by rule …road.lane.center_lane_no_width;
  // remove_lane only accepts the outermost lane of a side (M2 restriction).
  width_spin_->setEnabled(lane_selected && !center && !lane->widths.empty());
  remove_lane_->setEnabled(lane_selected && !center && is_outermost(document_.network(), *lane));
  // Lane 0's mark IS the center-line style — always editable on a lane.
  mark_combo_->setEnabled(lane_selected);
  mark_width_spin_->setEnabled(lane_selected);

  if (!lane_selected) {
    return;
  }
  rebuild_choice_combo(*type_combo_, kTypeChoices, lane->type, lane_type_name);
  if (!lane->widths.empty()) {
    const QSignalBlocker blocker(width_spin_);
    width_spin_->setValue(lane->widths.front().a);
  }
  const RoadMark mark = lane->road_marks.empty() ? RoadMark{} : lane->road_marks.front();
  rebuild_choice_combo(*mark_combo_, kMarkChoices, mark.type, mark_type_name);
  {
    const QSignalBlocker blocker(mark_width_spin_);
    mark_width_spin_->setValue(mark.width);
  }
}

void PropertiesPanel::push(std::unique_ptr<edit::Command> command) {
  // A refused command appends a document diagnostic (push_command contract)
  // and the panel re-syncs from the unchanged network via mesh_changed —
  // nothing further to do here.
  static_cast<void>(document_.push_command(std::move(command)));
}

const Lane* PropertiesPanel::primary_lane() const {
  return document_.network().lane(selection_.primary().lane);
}

LaneSectionId PropertiesPanel::target_section() const {
  if (const Lane* lane = primary_lane()) {
    return lane->section;
  }
  const Road* road = document_.network().road(selection_.primary().road);
  return road != nullptr && !road->sections.empty() ? road->sections.front() : LaneSectionId{};
}

void PropertiesPanel::add_row(const QString& label, const QString& value) {
  auto* value_label = new QLabel(value, this);
  value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form_->addRow(label, value_label);
}

void PropertiesPanel::clear_rows() {
  while (form_->rowCount() > 0) {
    form_->removeRow(0);
  }
}

} // namespace roadmaker::editor
