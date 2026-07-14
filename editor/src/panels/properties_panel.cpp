#include "panels/properties_panel.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <utility>

#include "tools/elevation_tool.hpp"

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

/// The outermost lane on `side` (>0 left, <0 right) of the section, or an
/// invalid id when the side carries no lane. This is the only removable
/// position in M2 (keeps OpenDRIVE numbering contiguous), so the per-side
/// Remove buttons act on it directly — no lane selection required.
LaneId outermost_lane_of_side(const RoadNetwork& network, LaneSectionId section_id, int side) {
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return LaneId{};
  }
  LaneId best;
  int best_odr = 0;
  for (const LaneId id : section->lanes) {
    const int odr = network.lane(id)->odr_id;
    const bool on_side = side > 0 ? odr > 0 : odr < 0;
    if (on_side && (!best.is_valid() || (side > 0 ? odr > best_odr : odr < best_odr))) {
      best = id;
      best_odr = odr;
    }
  }
  return best;
}

/// Lanes on `side` (>0 left, <0 right) of the section.
std::size_t lane_count_on_side(const RoadNetwork& network, LaneSectionId section_id, int side) {
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return 0;
  }
  std::size_t count = 0;
  for (const LaneId id : section->lanes) {
    const int odr = network.lane(id)->odr_id;
    if (side > 0 ? odr > 0 : odr < 0) {
      ++count;
    }
  }
  return count;
}

} // namespace

PropertiesPanel::PropertiesPanel(Document& document,
                                 const SelectionModel& selection,
                                 QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection), form_(new QFormLayout),
      placeholder_(new QLabel(tr("Select a road or lane."), this)), name_row_(new QWidget(this)),
      name_edit_(new QLineEdit), lane_group_(new QGroupBox(tr("Lane profile"), this)),
      type_combo_(new QComboBox), width_spin_(new QDoubleSpinBox), mark_combo_(new QComboBox),
      mark_width_spin_(new QDoubleSpinBox), add_left_(new QPushButton(tr("Add left"))),
      add_right_(new QPushButton(tr("Add right"))),
      remove_left_(new QPushButton(tr("Remove left lane"))),
      remove_right_(new QPushButton(tr("Remove right lane"))),
      elevation_group_(new QGroupBox(tr("Elevation"), this)),
      elevation_node_label_(new QLabel(this)), elevation_spin_(new QDoubleSpinBox) {
  placeholder_->setWordWrap(true);
  placeholder_->setEnabled(false);

  // The editable name row persists across refreshes (form_ rows are
  // rebuilt from scratch, which would destroy an editor placed there).
  name_edit_->setObjectName(QStringLiteral("road_name_edit"));
  auto* name_form = new QFormLayout(name_row_);
  name_form->setContentsMargins(0, 0, 0, 0);
  name_form->addRow(tr("Name"), name_edit_);

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
  remove_left_->setObjectName(QStringLiteral("remove_left_lane_button"));
  remove_right_->setObjectName(QStringLiteral("remove_right_lane_button"));

  auto* lane_form = new QFormLayout;
  lane_form->addRow(tr("Type"), type_combo_);
  lane_form->addRow(tr("Width"), width_spin_);
  lane_form->addRow(tr("Road mark"), mark_combo_);
  lane_form->addRow(tr("Mark width"), mark_width_spin_);
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(add_left_);
  buttons->addWidget(add_right_);
  auto* remove_buttons = new QHBoxLayout;
  remove_buttons->addWidget(remove_left_);
  remove_buttons->addWidget(remove_right_);
  auto* group_layout = new QVBoxLayout(lane_group_);
  group_layout->addLayout(lane_form);
  group_layout->addLayout(buttons);
  group_layout->addLayout(remove_buttons);

  // Elevation: one persistent spin box that edits whichever node the
  // Elevation tool has made active (issue #16, spec 02 §5). The label names
  // the node; the spin box drives edit::set_node_elevation.
  elevation_spin_->setObjectName(QStringLiteral("node_elevation_spin"));
  elevation_spin_->setRange(-1000.0, 1000.0);
  elevation_spin_->setSingleStep(0.5);
  elevation_spin_->setDecimals(3);
  elevation_spin_->setSuffix(tr(" m"));
  elevation_node_label_->setWordWrap(true);
  auto* elevation_form = new QFormLayout(elevation_group_);
  elevation_form->addRow(elevation_node_label_);
  elevation_form->addRow(tr("Height"), elevation_spin_);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(placeholder_);
  layout->addWidget(name_row_);
  layout->addLayout(form_);
  layout->addWidget(lane_group_);
  layout->addWidget(elevation_group_);
  layout->addStretch();

  // One command per discrete action (spec 01 §7). Combos commit on
  // `activated` — user gestures only, never programmatic refresh; text/spin
  // editors on editingFinished, skipping pushes when the value did not
  // change — that skip is the re-entrancy guard: refresh() re-syncs the
  // editors from the network without a command echoing back.
  connect(name_edit_, &QLineEdit::editingFinished, this, [this] {
    const Road* road = document_.network().road(selection_.primary().road);
    if (road == nullptr || name_edit_->text().toStdString() == road->name) {
      return;
    }
    push(edit::rename_road(
        document_.network(), selection_.primary().road, name_edit_->text().toStdString()));
  });
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
  connect(remove_left_, &QPushButton::clicked, this, [this] { remove_outermost_lane(+1); });
  connect(remove_right_, &QPushButton::clicked, this, [this] { remove_outermost_lane(-1); });
  // Commit the active node's height on focus-out, skipping the push when the
  // value is unchanged — the same re-entrancy guard the lane editors use, so
  // refresh() re-syncing the spin box after undo never echoes a command back.
  connect(elevation_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (elevation_tool_ == nullptr) {
      return;
    }
    const auto node = elevation_tool_->active_node();
    if (!node.has_value()) {
      return;
    }
    const Road* road = document_.network().road(node->first);
    if (road == nullptr) {
      return;
    }
    const auto stations = edit::waypoint_stations(*road);
    if (!stations.has_value() || node->second >= stations->size()) {
      return;
    }
    const double current = eval_profile(road->elevation, (*stations)[node->second]);
    if (std::abs(elevation_spin_->value() - current) < 1e-9) {
      return;
    }
    push(edit::set_node_elevation(
        document_.network(), node->first, node->second, elevation_spin_->value()));
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
    name_row_->hide();
    lane_group_->hide();
    elevation_group_->hide();
    // A selected junction floor has no road but is a real entity — show its
    // topology instead of the empty placeholder (gate finding 4).
    if (const Junction* junction = document_.network().junction(primary.junction)) {
      placeholder_->hide();
      add_row(tr("OpenDRIVE id"), tr("junction %1").arg(QString::fromStdString(junction->odr_id)));
      add_row(tr("Arms"), QString::number(junction->arms.size()));
      add_row(tr("Connections"), QString::number(junction->connections.size()));
      return;
    }
    placeholder_->show();
    return;
  }
  placeholder_->hide();

  name_row_->show();
  {
    const QSignalBlocker blocker(name_edit_);
    name_edit_->setText(QString::fromStdString(road->name));
    name_edit_->setPlaceholderText(tr("Road %1").arg(QString::fromStdString(road->odr_id)));
  }

  if (selection_.entries().size() > 1) {
    add_row(tr("Selection"), tr("%1 items").arg(selection_.entries().size()));
  }
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
  refresh_elevation();
}

void PropertiesPanel::set_elevation_tool(ElevationTool* tool) {
  elevation_tool_ = tool;
  if (elevation_tool_ != nullptr) {
    connect(elevation_tool_,
            &ElevationTool::active_node_changed,
            this,
            &PropertiesPanel::refresh_elevation);
  }
  refresh_elevation();
}

void PropertiesPanel::refresh_elevation() {
  // The section only appears once an Elevation tool is wired up; without an
  // active node it prompts the user to pick one in the viewport.
  if (elevation_tool_ == nullptr ||
      document_.network().road(selection_.primary().road) == nullptr) {
    elevation_group_->hide();
    return;
  }
  elevation_group_->show();

  const auto node = elevation_tool_->active_node();
  const Road* road = node.has_value() ? document_.network().road(node->first) : nullptr;
  if (road != nullptr) {
    if (const auto stations = edit::waypoint_stations(*road);
        stations.has_value() && node->second < stations->size()) {
      elevation_spin_->setEnabled(true);
      elevation_node_label_->setText(
          tr("Node %1 (s = %2 m)").arg(node->second).arg((*stations)[node->second], 0, 'f', 2));
      const QSignalBlocker blocker(elevation_spin_);
      elevation_spin_->setValue(eval_profile(road->elevation, (*stations)[node->second]));
      return;
    }
  }
  elevation_spin_->setEnabled(false);
  elevation_node_label_->setText(tr("Click a road node to edit its elevation."));
}

void PropertiesPanel::refresh_lane_section() {
  const Lane* lane = primary_lane();
  const bool lane_selected = lane != nullptr;
  const bool center = lane_selected && lane->odr_id == 0;

  // Add-lane buttons act on the section, so a road-level selection suffices.
  add_left_->setEnabled(true);
  add_right_->setEnabled(true);

  type_combo_->setEnabled(lane_selected && !center);
  // The center lane is width-less by rule …road.lane.center_lane_no_width.
  width_spin_->setEnabled(lane_selected && !center && !lane->widths.empty());
  // Per-side Remove acts on the section's outermost lane on that side — no
  // lane selection required (the discoverable lane-removal affordance, gate
  // finding 6). The kernel only removes the outermost lane; the editor also
  // protects the last driving lane so a side can't be emptied of its road.
  const auto configure_remove = [this](QPushButton* button, int side) {
    const LaneSectionId section = target_section();
    if (document_.network().road(selection_.primary().road) == nullptr) {
      button->setEnabled(false);
      button->setToolTip(tr("Select a road first."));
      return;
    }
    const LaneId outermost = outermost_lane_of_side(document_.network(), section, side);
    if (!outermost.is_valid()) {
      button->setEnabled(false);
      button->setToolTip(tr("No lane on this side to remove."));
      return;
    }
    const bool sole_driving = lane_count_on_side(document_.network(), section, side) == 1 &&
                              document_.network().lane(outermost)->type == LaneType::Driving;
    button->setEnabled(!sole_driving);
    button->setToolTip(sole_driving ? tr("Only the driving lane remains.")
                                    : tr("Remove the outermost lane on this side."));
  };
  configure_remove(remove_left_, +1);
  configure_remove(remove_right_, -1);
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

void PropertiesPanel::remove_outermost_lane(int side) {
  const LaneId lane_id = outermost_lane_of_side(document_.network(), target_section(), side);
  const Lane* lane = document_.network().lane(lane_id);
  if (lane == nullptr) {
    return;
  }
  const int odr = lane->odr_id;
  if (document_.push_command(edit::remove_lane(document_.network(), lane_id))) {
    emit status_message(tr("Removed lane %1.").arg(odr));
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
