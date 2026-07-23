// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "panels/properties_panel.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "document/crosswalk_item.hpp"
#include "document/library_drop.hpp"
#include "document/library_list_model.hpp"
#include "document/library_manifest.hpp"
#include "document/marking_item.hpp"
#include "render/material_catalog.hpp"
#include "tools/corner_tool.hpp"
#include "tools/elevation_tool.hpp"
#include "tools/junction_surface_tool.hpp"
#include "tools/maneuver_tool.hpp"
#include "tools/signal_tool.hpp"
#include "tools/stopline_tool.hpp"

namespace roadmaker::editor {

namespace {

/// One maneuver of `junction`, solved against the network handed in. Every row
/// editor re-reads its baseline through this rather than through a value
/// captured when the row was built: a refresh re-seed must not echo a command
/// back, and the kernel refuses a no-op outright.
std::optional<JunctionManeuverInfo>
maneuver_info(const RoadNetwork& network, JunctionId junction, RoadId road) {
  for (const JunctionManeuverInfo& info : junction_maneuvers(network, junction)) {
    if (info.road == road) {
      return info;
    }
  }
  return std::nullopt;
}

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

/// Total surface area [m²] of a triangle mesh: half the summed magnitude of each
/// triangle's edge cross product. Read-only display for a ground surface (#215).
double mesh_area(const SubMesh& mesh) {
  double area = 0.0;
  const auto pos = [&](std::uint32_t i, std::size_t axis) {
    return mesh.positions[(static_cast<std::size_t>(i) * 3) + axis];
  };
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::uint32_t a = mesh.indices[i];
    const std::uint32_t b = mesh.indices[i + 1];
    const std::uint32_t c = mesh.indices[i + 2];
    const double e1x = pos(b, 0) - pos(a, 0);
    const double e1y = pos(b, 1) - pos(a, 1);
    const double e1z = pos(b, 2) - pos(a, 2);
    const double e2x = pos(c, 0) - pos(a, 0);
    const double e2y = pos(c, 1) - pos(a, 1);
    const double e2z = pos(c, 2) - pos(a, 2);
    const double cx = (e1y * e2z) - (e1z * e2y);
    const double cy = (e1z * e2x) - (e1x * e2z);
    const double cz = (e1x * e2y) - (e1y * e2x);
    area += 0.5 * std::sqrt((cx * cx) + (cy * cy) + (cz * cz));
  }
  return area;
}

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

/// Display name for an object's OpenDRIVE @type (§13.1, Table 85).
QString object_type_name(ObjectType type) {
  switch (type) {
  case ObjectType::Crosswalk:
    return QStringLiteral("Crosswalk");
  case ObjectType::Tree:
    return QStringLiteral("Tree");
  case ObjectType::Vegetation:
    return QStringLiteral("Vegetation");
  case ObjectType::Pole:
    return QStringLiteral("Pole");
  case ObjectType::Barrier:
    return QStringLiteral("Barrier");
  case ObjectType::Building:
    return QStringLiteral("Building");
  case ObjectType::Obstacle:
    return QStringLiteral("Obstacle");
  case ObjectType::None:
    return QStringLiteral("Untyped");
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

/// Smallest fillet radius the Corner spin box offers [m]. Below this the fillet
/// stops reading as a corner at all; the UPPER bound is per-corner geometry
/// (JunctionCornerInfo::max_radius) and is applied on every refresh.
constexpr double kCornerRadiusMin = 0.5;

/// Human-readable name of one junction arm: its road's OpenDRIVE id plus the
/// end that contacts the junction. A corner is named by its arm PAIR, and both
/// ends of one road can be arms of the same junction, so the contact point is
/// part of the identity, not decoration.
QString arm_name(const RoadNetwork& network, const RoadEnd& end) {
  const Road* road = network.road(end.road);
  const QString id = road == nullptr ? QObject::tr("(gone)") : QString::fromStdString(road->odr_id);
  return end.contact == ContactPoint::Start ? QObject::tr("road %1 start").arg(id)
                                            : QObject::tr("road %1 end").arg(id);
}

} // namespace

PropertiesPanel::PropertiesPanel(Document& document,
                                 const SelectionModel& selection,
                                 QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection), form_(new QFormLayout),
      placeholder_(new QLabel(tr("Select a road or lane."), this)), name_row_(new QWidget(this)),
      name_edit_(new QLineEdit), lane_group_(new QGroupBox(tr("Lane profile"), this)),
      type_combo_(new QComboBox), width_spin_(new QDoubleSpinBox), mark_combo_(new QComboBox),
      mark_width_spin_(new QDoubleSpinBox),
      marking_slot_(new SlotWidget(QStringLiteral("Markings"), this)),
      lane_material_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      add_left_(new QPushButton(tr("Add left"))), add_right_(new QPushButton(tr("Add right"))),
      remove_left_(new QPushButton(tr("Remove left lane"))),
      remove_right_(new QPushButton(tr("Remove right lane"))),
      elevation_group_(new QGroupBox(tr("Elevation"), this)),
      elevation_node_label_(new QLabel(this)), elevation_spin_(new QDoubleSpinBox),
      corner_group_(new QGroupBox(tr("Corner"), this)), corner_arms_label_(new QLabel(this)),
      corner_radius_spin_(new QDoubleSpinBox),
      corner_sidewalk_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      corner_median_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      stopline_group_(new QGroupBox(tr("Stop line"), this)), stopline_arm_label_(new QLabel(this)),
      stopline_distance_spin_(new QDoubleSpinBox),
      stopline_flip_button_(new QPushButton(tr("Flip direction"))),
      stopline_reset_button_(new QPushButton(tr("Reset to default"))),
      surface_spans_group_(new QGroupBox(tr("Surface spans"), this)),
      maneuvers_group_(new QGroupBox(tr("Maneuvers"), this)),
      signalization_group_(new QGroupBox(tr("Signalization"), this)),
      junction_group_(new QGroupBox(tr("Junction"), this)), junction_type_label_(new QLabel(this)),
      junction_locked_check_(new QCheckBox(tr("Locked"), this)),
      junction_radius_spin_(new QDoubleSpinBox),
      junction_material_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      signal_group_(new QGroupBox(tr("Signal"), this)), signal_s_spin_(new QDoubleSpinBox),
      signal_t_spin_(new QDoubleSpinBox), signal_h_spin_(new QDoubleSpinBox),
      signal_kind_label_(new QLabel(this)), signal_text_edit_(new QPlainTextEdit),
      object_group_(new QGroupBox(tr("Prop"), this)), object_kind_label_(new QLabel(this)),
      model_slot_(new SlotWidget(QStringLiteral("Props"), this)),
      instance_material_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      object_height_spin_(new QDoubleSpinBox), style_group_(new QGroupBox(tr("Road style"), this)),
      style_slot_(new SlotWidget(QStringLiteral("Road styles"), this)),
      surface_group_(new QGroupBox(tr("Ground surface"), this)),
      material_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      asset_group_(new QGroupBox(tr("Crosswalk asset"), this)),
      asset_width_spin_(new QDoubleSpinBox), asset_border_spin_(new QDoubleSpinBox),
      asset_dash_spin_(new QDoubleSpinBox), asset_gap_spin_(new QDoubleSpinBox),
      asset_material_slot_(new SlotWidget(QStringLiteral("Materials"), this)),
      asset_category_edit_(new QLineEdit), asset_preview_(new QLabel(this)),
      asset_hint_(new QLabel(this)), prop_set_group_(new QGroupBox(tr("Prop set"), this)),
      prop_set_hint_(new QLabel(this)), prop_group_(new QGroupBox(tr("Prop asset"), this)),
      prop_model_label_(new QLabel(this)), prop_scale_spin_(new QDoubleSpinBox),
      prop_scale_hint_(new QLabel(this)), prop_hint_(new QLabel(this)) {
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

  // Scrub-editing: the NAME of a numeric attribute is the drag handle
  // (P1/GW-2). Each label owns one binding; the gesture is one preview session
  // and therefore exactly one undo entry. The per-attribute rates below are
  // editor conventions, chosen so a ~100 px drag covers a useful range: 2 m of
  // lane width, 0.2 m of mark width, 5 m of height, 10 m along s.
  auto* lane_form = new QFormLayout;
  lane_form->addRow(tr("Type"), type_combo_);
  width_scrub_label_ = install_scrub(
      new ScrubLabel(tr("Width"), this),
      {.spin = width_spin_,
       .units_per_pixel = 0.02,
       .baseline = [this]() -> std::optional<double> {
         const Lane* lane = primary_lane();
         // A tapered lane's width is edited on the 2D Width curve, not
         // here: a nullopt baseline makes begin_scrub treat the drag
         // as inert, so set_lane_width (which would refuse anyway) is
         // never reached and no taper is flattened.
         if (lane == nullptr || lane->widths.empty() || !lane_width_is_constant(*lane)) {
           return std::nullopt;
         }
         return lane->widths.front().a;
       },
       .factory =
           [this](const RoadNetwork& network, double value) {
             return edit::set_lane_width(network, selection_.primary().lane, value);
           }});
  width_scrub_label_->setObjectName(QStringLiteral("lane_width_scrub"));
  lane_form->addRow(width_scrub_label_, width_spin_);
  lane_form->addRow(tr("Road mark"), mark_combo_);
  lane_form->addRow(
      install_scrub(new ScrubLabel(tr("Mark width"), this),
                    {.spin = mark_width_spin_,
                     .units_per_pixel = 0.002,
                     .baseline = [this]() -> std::optional<double> {
                       const Lane* lane = primary_lane();
                       if (lane == nullptr) {
                         return std::nullopt;
                       }
                       return lane->road_marks.empty() ? kMarkWidthStandard
                                                       : lane->road_marks.front().width;
                     },
                     .factory =
                         [this](const RoadNetwork& network, double value) {
                           const Lane* lane = primary_lane();
                           RoadMark mark = lane == nullptr || lane->road_marks.empty()
                                               ? RoadMark{}
                                               : lane->road_marks.front();
                           mark.type =
                               static_cast<RoadMarkType>(mark_combo_->currentData().toInt());
                           mark.width = value;
                           return edit::set_road_mark(network, selection_.primary().lane, mark);
                         }}),
      mark_width_spin_);
  // Lane marking slot (p3-s1): drop a Markings item here (or onto the lane
  // boundary in the viewport) to set this lane's road mark — the slot and the
  // combo edit the same <roadMark>, but the slot reaches variants the combo's
  // Solid/Broken/None shortlist does not (double-yellow, dashed, edge lines).
  marking_slot_->setObjectName(QStringLiteral("lane_marking_slot"));
  marking_slot_->setToolTip(tr("Drop a marking to set this lane's road mark"));
  lane_form->addRow(tr("Marking"), marking_slot_);
  connect(marking_slot_, &SlotWidget::item_dropped, this, &PropertiesPanel::push_marking);
  connect(marking_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
  // Lane Materials slot (p6-s3): drop a material here (or onto the lane in the
  // viewport) to pave the selected lane's surface. Write-only, like the road-
  // style slot; disabled for the centre lane in refresh_lane_section.
  lane_material_slot_->setObjectName(QStringLiteral("lane_material_slot"));
  lane_material_slot_->setToolTip(tr("Drop a material to pave this lane's surface"));
  lane_form->addRow(tr("Material"), lane_material_slot_);
  connect(
      lane_material_slot_, &SlotWidget::item_dropped, this, &PropertiesPanel::push_lane_material);
  connect(lane_material_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
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
  elevation_form->addRow(
      install_scrub(new ScrubLabel(tr("Height"), this),
                    {.spin = elevation_spin_,
                     .units_per_pixel = 0.05,
                     .baseline = [this]() -> std::optional<double> { return active_node_height(); },
                     .factory =
                         [this](const RoadNetwork& network, double value) {
                           const auto node = elevation_tool_->active_node();
                           return edit::set_node_elevation(
                               network, node->first, node->second, value);
                         }}),
      elevation_spin_);

  // Corner: the fillet radius of whichever junction corner the Corner tool has
  // made active (p4-s1, GW-2 s9 / GW-3 s5). The tool's sub-selection is not a
  // SelectionModel entry, so the arm-pair row is the only place the pane can
  // say WHICH corner these numbers belong to.
  corner_arms_label_->setObjectName(QStringLiteral("corner_arms_label"));
  corner_arms_label_->setWordWrap(true);
  corner_radius_spin_->setObjectName(QStringLiteral("corner_radius_spin"));
  // The upper bound is per-corner geometry (refresh_corner re-ranges it); this
  // is just the floor a fillet stays meaningful at.
  corner_radius_spin_->setRange(kCornerRadiusMin, kCornerRadiusMin);
  corner_radius_spin_->setSingleStep(0.5);
  corner_radius_spin_->setDecimals(2);
  corner_radius_spin_->setSuffix(tr(" m"));
  corner_form_ = new QFormLayout(corner_group_);
  corner_form_->addRow(tr("Arms"), corner_arms_label_);
  corner_radius_scrub_label_ =
      install_scrub(new ScrubLabel(tr("Corner radius"), this),
                    {.spin = corner_radius_spin_,
                     .units_per_pixel = 0.05,
                     .baseline = [this]() -> std::optional<double> {
                       const std::optional<JunctionCornerInfo> info = active_corner_info();
                       return info.has_value() ? std::optional<double>(info->radius) : std::nullopt;
                     },
                     .factory =
                         [this](const RoadNetwork& network, double value) {
                           const auto corner = corner_tool_->active_corner();
                           return edit::set_corner_radius(
                               network, corner->junction, corner->arm_a, corner->arm_b, value);
                         }});
  corner_radius_scrub_label_->setObjectName(QStringLiteral("corner_radius_scrub"));
  corner_form_->addRow(corner_radius_scrub_label_, corner_radius_spin_);
  // Per-corner overlay materials (p4-s2): the sidewalk wedge and the median
  // nose. Both reflect the stored value (empty → the slot's placeholder, which
  // is also the truth: an unpainted corner meshes no overlay at all).
  corner_sidewalk_slot_->setObjectName(QStringLiteral("corner_sidewalk_slot"));
  corner_sidewalk_slot_->setToolTip(tr("Drop a material to pave this corner's sidewalk wedge"));
  corner_form_->addRow(tr("Sidewalk"), corner_sidewalk_slot_);
  connect(corner_sidewalk_slot_,
          &SlotWidget::item_dropped,
          this,
          &PropertiesPanel::push_corner_sidewalk_material);
  connect(corner_sidewalk_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
  corner_median_slot_->setObjectName(QStringLiteral("corner_median_slot"));
  corner_median_slot_->setToolTip(tr("Drop a material to pave this corner's median nose"));
  corner_form_->addRow(tr("Median"), corner_median_slot_);
  connect(corner_median_slot_,
          &SlotWidget::item_dropped,
          this,
          &PropertiesPanel::push_corner_median_material);
  connect(corner_median_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
  corner_group_->hide();

  // Stop line: the setback of whichever junction stop line the Stop Line tool
  // has made active (p4-s3, GW-5). Every arm HAS a line — they are derived — so
  // this section only ever edits, and "Reset to default" is what un-authors one.
  stopline_arm_label_->setObjectName(QStringLiteral("stopline_arm_label"));
  stopline_arm_label_->setWordWrap(true);
  stopline_distance_spin_->setObjectName(QStringLiteral("stopline_distance_spin"));
  // The upper bound is per-arm geometry (refresh_stopline re-ranges it). Zero is
  // a MEANINGFUL setback here — a line right at the mouth — so unlike the
  // junction radius default there is no special-value sentinel and no clamp
  // away from zero.
  stopline_distance_spin_->setRange(0.0, 0.0);
  stopline_distance_spin_->setSingleStep(0.5);
  stopline_distance_spin_->setDecimals(2);
  stopline_distance_spin_->setSuffix(tr(" m"));
  stopline_form_ = new QFormLayout(stopline_group_);
  stopline_form_->addRow(tr("Arm"), stopline_arm_label_);
  stopline_distance_scrub_label_ = install_scrub(
      new ScrubLabel(tr("Distance"), this),
      {.spin = stopline_distance_spin_,
       .units_per_pixel = 0.05,
       .baseline = [this]() -> std::optional<double> {
         const std::optional<JunctionStopLineInfo> info = active_stopline_info();
         return info.has_value() ? std::optional<double>(info->distance) : std::nullopt;
       },
       .factory =
           [this](const RoadNetwork& network, double value) {
             const auto line = stopline_tool_->active_stopline();
             return edit::set_stopline_distance(network, line->junction, line->arm, value);
           }});
  stopline_distance_scrub_label_->setObjectName(QStringLiteral("stopline_distance_scrub"));
  stopline_form_->addRow(stopline_distance_scrub_label_, stopline_distance_spin_);

  stopline_flip_button_->setObjectName(QStringLiteral("stopline_flip_button"));
  stopline_flip_button_->setToolTip(
      tr("Span the outgoing lanes instead of the approach ones (F in the Stop Line tool)"));
  connect(stopline_flip_button_, &QPushButton::clicked, this, [this] {
    const auto line = stopline_tool_ != nullptr ? stopline_tool_->active_stopline() : std::nullopt;
    if (!line.has_value()) {
      return;
    }
    push(edit::flip_stopline(document_.network(), line->junction, line->arm));
  });
  stopline_form_->addRow(QString(), stopline_flip_button_);

  stopline_reset_button_->setObjectName(QStringLiteral("stopline_reset_button"));
  stopline_reset_button_->setToolTip(
      tr("Drop this arm's authored stop line and return it to the derived default"));
  connect(stopline_reset_button_, &QPushButton::clicked, this, [this] {
    const auto line = stopline_tool_ != nullptr ? stopline_tool_->active_stopline() : std::nullopt;
    if (!line.has_value()) {
      return;
    }
    push(edit::reset_stopline(document_.network(), line->junction, line->arm));
  });
  stopline_form_->addRow(QString(), stopline_reset_button_);
  stopline_group_->hide();

  // Surface spans (p4-s5, issue #320): one row per connecting road of the
  // selected junction's floor. Dynamic PLAIN widgets rebuilt on every refresh —
  // deliberately not a QAbstractItemModel, because the rows are neither
  // sortable nor drag-reorderable and there is nothing to share with a view.
  // This is the first reorder UI in the editor, and it is intentionally minimal:
  // paired Raise/Lower buttons (the stop line's flip/reset precedent), no
  // drag handles.
  surface_spans_group_->setObjectName(QStringLiteral("surface_spans_group"));
  surface_spans_layout_ = new QVBoxLayout(surface_spans_group_);
  surface_spans_layout_->setSpacing(2);
  surface_spans_group_->hide();

  // Maneuvers (p4-s6, issue #227): one row per connecting road of the selected
  // junction, plus the junction-wide Rebuild. Same dynamic-plain-widget shape
  // as the span rows above, and for the same reasons.
  maneuvers_group_->setObjectName(QStringLiteral("maneuvers_group"));
  auto* maneuvers_column = new QVBoxLayout(maneuvers_group_);
  maneuvers_column->setSpacing(2);
  // The rows live in their own container so the junction-wide Rebuild button
  // below survives the wholesale row rebuild.
  auto* maneuver_rows = new QWidget(maneuvers_group_);
  maneuvers_layout_ = new QVBoxLayout(maneuver_rows);
  maneuvers_layout_->setContentsMargins(0, 0, 0, 0);
  maneuvers_layout_->setSpacing(2);
  maneuvers_column->addWidget(maneuver_rows);
  maneuvers_rebuild_button_ = new QPushButton(tr("Rebuild Maneuvers"), maneuvers_group_);
  maneuvers_rebuild_button_->setObjectName(QStringLiteral("maneuver_rebuild_button"));
  maneuvers_rebuild_button_->setToolTip(
      tr("Replan every turn of this junction from its arms, discarding hand-shaped geometry and "
         "per-turn locks. Turn-type overrides survive — they are semantic, not geometric."));
  connect(maneuvers_rebuild_button_, &QPushButton::clicked, this, [this] {
    // The junction is read at CLICK time from the live selection, never from a
    // value captured when the row was built.
    const JunctionId junction = selection_.primary().junction;
    if (!junction.is_valid()) {
      return;
    }
    push(edit::rebuild_maneuvers(document_.network(), junction));
  });
  maneuvers_column->addWidget(maneuvers_rebuild_button_);
  maneuvers_group_->hide();

  // Signalization (p4-s7, issue #228): the junction-wide template controls plus
  // one read-only row per approach. The combos edit the SIGNAL TOOL's pending
  // state — the panel caches nothing, so undo/redo and a context-menu
  // signalization re-seed it like any other refresh.
  signalization_group_->setObjectName(QStringLiteral("signalization_group"));
  auto* signalization_column = new QVBoxLayout(signalization_group_);
  signalization_column->setSpacing(2);

  signalization_template_combo_ = new QComboBox(signalization_group_);
  signalization_template_combo_->setObjectName(QStringLiteral("signalization_template_combo"));
  // Index == edit::SignalizeTemplate's own order, so no lookup table is needed
  // (the same one-to-one mapping signalize_template_token relies on).
  signalization_template_combo_->addItem(tr("Protected left (4-phase)"));
  signalization_template_combo_->addItem(tr("Two phase (permissive lefts)"));
  signalization_template_combo_->addItem(tr("All-way stop"));
  signalization_template_combo_->addItem(tr("Two-way stop"));
  signalization_template_combo_->setToolTip(
      tr("Which plan Auto Signalize applies. The two stop templates are static: they place signs "
         "and create no controllers at all, because an all-way stop has no phases."));
  connect(signalization_template_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (signal_tool_ == nullptr || index < 0) {
      return;
    }
    signal_tool_->set_pending_template(static_cast<edit::SignalizeTemplate>(index));
    refresh_signalization();
  });
  signalization_column->addWidget(signalization_template_combo_);

  signalization_mount_combo_ = new QComboBox(signalization_group_);
  signalization_mount_combo_->setObjectName(QStringLiteral("signalization_mount_combo"));
  signalization_mount_combo_->addItem(tr("No mount prop"), QString());
  for (const std::string& id : props::ids()) {
    const QString model = QString::fromStdString(id);
    signalization_mount_combo_->addItem(model, model);
  }
  signalization_mount_combo_->setToolTip(
      tr("An optional prop placed with each head and recorded as its physical mount. The record "
         "already holds a LIST of parts, so an assembly slots in unchanged."));
  connect(signalization_mount_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (signal_tool_ == nullptr || index < 0) {
      return;
    }
    signal_tool_->set_pending_mount_model(
        signalization_mount_combo_->itemData(index).toString().toStdString());
    refresh_signalization();
  });
  signalization_column->addWidget(signalization_mount_combo_);

  signalization_apply_button_ = new QPushButton(tr("Auto Signalize"), signalization_group_);
  signalization_apply_button_->setObjectName(QStringLiteral("signalization_apply_button"));
  connect(signalization_apply_button_, &QPushButton::clicked, this, [this] {
    if (signal_tool_ != nullptr) {
      static_cast<void>(signal_tool_->signalize());
    }
  });
  signalization_column->addWidget(signalization_apply_button_);

  signalization_clear_button_ = new QPushButton(tr("Clear Signalization"), signalization_group_);
  signalization_clear_button_->setObjectName(QStringLiteral("signalization_clear_button"));
  signalization_clear_button_->setToolTip(
      tr("Erase the signals, controllers and mount props this junction's signalization authored. "
         "A hand-placed sign of another type is left alone."));
  connect(signalization_clear_button_, &QPushButton::clicked, this, [this] {
    if (signal_tool_ != nullptr) {
      static_cast<void>(signal_tool_->clear());
    }
  });
  signalization_column->addWidget(signalization_clear_button_);

  // The approach rows live in their own container so the controls above survive
  // the wholesale row rebuild.
  auto* signalization_rows = new QWidget(signalization_group_);
  signalization_rows_layout_ = new QVBoxLayout(signalization_rows);
  signalization_rows_layout_->setContentsMargins(0, 0, 0, 0);
  signalization_rows_layout_->setSpacing(2);
  signalization_column->addWidget(signalization_rows);
  signalization_group_->hide();

  // Junction: the junction-WIDE values (p4-s2). The radius default is the
  // fallback every corner without its own radius uses, so its zero position is
  // "no default at all" — spelled "Derived" rather than 0 m, and reachable only
  // by typing (a scrub clamps to the editable floor so a drag never clears it).
  junction_radius_spin_->setObjectName(QStringLiteral("junction_corner_radius_spin"));
  junction_radius_spin_->setRange(0.0, 50.0);
  junction_radius_spin_->setSingleStep(0.5);
  junction_radius_spin_->setDecimals(2);
  junction_radius_spin_->setSuffix(tr(" m"));
  // Qt renders the special text ONLY at exactly the minimum, so the minimum
  // above must stay precisely 0.0 for this to ever appear.
  junction_radius_spin_->setSpecialValueText(tr("Derived"));
  junction_radius_spin_->setToolTip(
      tr("Fillet radius applied to every corner of this junction that has no radius of its own. "
         "Type 0 to clear it and return the corners to their derived radii."));
  auto* junction_form = new QFormLayout(junction_group_);
  // Junction state (p4-s4, issue #319). DERIVED, so it is read-only: the one
  // thing the user can change is the lock, and that is the checkbox below.
  junction_type_label_->setObjectName(QStringLiteral("junction_type_label"));
  junction_form->addRow(tr("Type"), junction_type_label_);
  junction_locked_check_->setObjectName(QStringLiteral("junction_locked_check"));
  junction_locked_check_->setToolTip(
      tr("A locked junction is skipped by the automatic regeneration loop, so hand-tuned "
         "connections, corners and stop lines survive edits to its arms. Re-derive it explicitly "
         "from the junction's context menu."));
  junction_form->addRow(QString(), junction_locked_check_);
  ScrubLabel* junction_radius_scrub = install_scrub(
      new ScrubLabel(tr("Corner radius"), this),
      {.spin = junction_radius_spin_,
       .units_per_pixel = 0.05,
       .baseline = [this]() -> std::optional<double> {
         const Junction* junction = document_.network().junction(selection_.primary().junction);
         if (junction == nullptr) {
           return std::nullopt;
         }
         return junction->default_corner_radius.value_or(0.0);
       },
       .factory =
           [this](const RoadNetwork& network, double value) {
             // A scrub must never emit the clear sentinel: dragging
             // to the floor sets the smallest real default, and
             // clearing stays a deliberate typed 0.
             return edit::set_junction_default_corner_radius(
                 network, selection_.primary().junction, std::max(value, kCornerRadiusMin));
           }});
  junction_radius_scrub->setObjectName(QStringLiteral("junction_corner_radius_scrub"));
  junction_form->addRow(junction_radius_scrub, junction_radius_spin_);
  junction_material_slot_->setObjectName(QStringLiteral("junction_material_slot"));
  junction_material_slot_->setToolTip(tr("Drop a material to pave this junction's carriageway"));
  junction_form->addRow(tr("Carriageway material"), junction_material_slot_);
  connect(junction_material_slot_,
          &SlotWidget::item_dropped,
          this,
          &PropertiesPanel::push_junction_material);
  connect(junction_material_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
  junction_group_->hide();

  // Signal: a placed <signal>'s road-relative pose. s/t/heading edit through
  // move_signal (one command each); type/subtype stay read-only for now (a
  // retype command is a later slice).
  signal_kind_label_->setObjectName(QStringLiteral("signal_kind_label"));
  signal_kind_label_->setWordWrap(true);
  const auto configure_signal_spin =
      [](QDoubleSpinBox* spin, const char* name, double lo, double hi, const QString& suffix) {
        spin->setObjectName(QString::fromLatin1(name));
        spin->setRange(lo, hi);
        spin->setDecimals(3);
        spin->setSuffix(suffix);
      };
  configure_signal_spin(signal_s_spin_, "signal_s_spin", 0.0, 100000.0, tr(" m"));
  signal_s_spin_->setSingleStep(1.0);
  configure_signal_spin(signal_t_spin_, "signal_t_spin", -100.0, 100.0, tr(" m"));
  signal_t_spin_->setSingleStep(0.5);
  configure_signal_spin(signal_h_spin_, "signal_h_spin", -6.2832, 6.2832, tr(" rad"));
  signal_h_spin_->setSingleStep(0.1);
  auto* signal_form = new QFormLayout(signal_group_);
  signal_form->addRow(signal_kind_label_);
  // The pose fields all commit through move_signal, so each scrub reads its two
  // siblings from their spin boxes — the selection cannot change mid-gesture.
  const auto signal_pose = [this](double s, double t, double h) {
    return [this, s, t, h](const RoadNetwork& network) {
      return edit::move_signal(network, selection_.selected_signals().back(), s, t, h);
    };
  };
  const auto primary_signal = [this]() -> const Signal* {
    const std::vector<SignalId> ids = selection_.selected_signals();
    return ids.empty() ? nullptr : document_.network().signal(ids.back());
  };
  signal_form->addRow(
      install_scrub(new ScrubLabel(tr("s"), this),
                    {.spin = signal_s_spin_,
                     .units_per_pixel = 0.1,
                     .baseline = [primary_signal]() -> std::optional<double> {
                       const Signal* signal = primary_signal();
                       return signal == nullptr ? std::nullopt : std::optional<double>(signal->s);
                     },
                     .factory =
                         [this, signal_pose](const RoadNetwork& network, double value) {
                           return signal_pose(
                               value, signal_t_spin_->value(), signal_h_spin_->value())(network);
                         }}),
      signal_s_spin_);
  signal_form->addRow(
      install_scrub(new ScrubLabel(tr("t"), this),
                    {.spin = signal_t_spin_,
                     .units_per_pixel = 0.05,
                     .baseline = [primary_signal]() -> std::optional<double> {
                       const Signal* signal = primary_signal();
                       return signal == nullptr ? std::nullopt : std::optional<double>(signal->t);
                     },
                     .factory =
                         [this, signal_pose](const RoadNetwork& network, double value) {
                           return signal_pose(
                               signal_s_spin_->value(), value, signal_h_spin_->value())(network);
                         }}),
      signal_t_spin_);
  signal_form->addRow(
      install_scrub(new ScrubLabel(tr("Heading offset"), this),
                    {.spin = signal_h_spin_,
                     .units_per_pixel = 0.01,
                     .baseline = [primary_signal]() -> std::optional<double> {
                       const Signal* signal = primary_signal();
                       return signal == nullptr ? std::nullopt
                                                : std::optional<double>(signal->h_offset);
                     },
                     .factory =
                         [this, signal_pose](const RoadNetwork& network, double value) {
                           return signal_pose(
                               signal_s_spin_->value(), signal_t_spin_->value(), value)(network);
                         }}),
      signal_h_spin_);
  // Editable face text (§14 Table 122). Compact multi-line editor; @text may
  // carry literal newlines, so it commits on focus-out (eventFilter), not Enter.
  signal_text_edit_->setObjectName(QStringLiteral("signal_text_edit"));
  signal_text_edit_->setPlaceholderText(tr("Face text (e.g. a town name)"));
  signal_text_edit_->setTabChangesFocus(true);
  signal_text_edit_->setFixedHeight(2 * signal_text_edit_->fontMetrics().lineSpacing() + 12);
  signal_text_edit_->installEventFilter(this);
  signal_form->addRow(tr("Text"), signal_text_edit_);

  // Prop: a placed <object> that renders a bundled prop model. The Model slot
  // takes a Library drag and re-points the prop at what was dropped.
  object_kind_label_->setObjectName(QStringLiteral("object_kind_label"));
  object_kind_label_->setWordWrap(true);
  model_slot_->setObjectName(QStringLiteral("object_model_slot"));
  instance_material_slot_->setObjectName(QStringLiteral("object_material_slot"));
  instance_material_slot_->setToolTip(
      tr("Drop a material to override this marking's paint on this instance only"));
  // Per-instance size (#335): the prop's OpenDRIVE @height. Dragging the label
  // resizes every selected prop live and commits ONE undo entry on release;
  // typing sets them all to that height. 500 m covers a skyscraper model.
  object_height_spin_->setObjectName(QStringLiteral("object_height_spin"));
  object_height_spin_->setRange(0.1, 500.0);
  object_height_spin_->setSingleStep(0.1);
  object_height_spin_->setDecimals(2);
  object_height_spin_->setSuffix(tr(" m"));
  object_height_spin_->setToolTip(
      tr("Rendered height of this prop, in meters. With several props selected, dragging scales "
         "them all by the same factor and typing sets them all to this height."));
  ScrubLabel* object_height_scrub =
      install_scrub(new ScrubLabel(tr("Height"), this),
                    {.spin = object_height_spin_,
                     .units_per_pixel = 0.05,
                     .baseline = [this]() -> std::optional<double> {
                       return primary_prop_effective_height(document_.network());
                     },
                     .factory =
                         [this](const RoadNetwork& network, double value) {
                           return resize_selected_props(network, value, /*absolute=*/false);
                         }});
  object_height_scrub->setObjectName(QStringLiteral("object_height_scrub"));
  auto* object_form = new QFormLayout(object_group_);
  object_form->addRow(object_kind_label_);
  object_form->addRow(tr("Model"), model_slot_);
  object_form->addRow(object_height_scrub, object_height_spin_);
  object_form->addRow(tr("Material"), instance_material_slot_);
  object_form_ = object_form;

  // Typing a height is the ABSOLUTE gesture: every selected prop becomes that
  // tall. Guarded against a scrub's live re-seed and against the refresh echo —
  // update_objects would happily record a no-op undo entry otherwise.
  connect(object_height_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (scrub_active_.has_value()) {
      return;
    }
    const std::optional<double> current = primary_prop_effective_height(document_.network());
    if (!current.has_value() || std::abs(object_height_spin_->value() - *current) < 1e-9) {
      return;
    }
    push(resize_selected_props(document_.network(),
                               object_height_spin_->value(),
                               /*absolute=*/true));
  });
  connect(model_slot_, &SlotWidget::item_dropped, this, &PropertiesPanel::push_object_model);
  connect(model_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);
  connect(instance_material_slot_,
          &SlotWidget::item_dropped,
          this,
          &PropertiesPanel::push_instance_material);
  connect(instance_material_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);

  // Road style: a write-only Library slot that re-styles the selected road.
  // Drop a style here or onto the road in the viewport — both apply it.
  style_slot_->setObjectName(QStringLiteral("road_style_slot"));
  style_slot_->setToolTip(tr("Drop a road style to replace this road's lane profile and markings"));
  auto* style_form = new QFormLayout(style_group_);
  style_form->addRow(tr("Style"), style_slot_);
  connect(style_slot_, &SlotWidget::item_dropped, this, &PropertiesPanel::push_road_style);
  connect(style_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);

  // Ground-surface Materials slot: drop a material here (or click to jump to the
  // Library) to re-texture the selected surface. The read-only stat rows are
  // rebuilt into form_ by refresh(); this group only carries the slot.
  material_slot_->setObjectName(QStringLiteral("surface_material_slot"));
  material_slot_->setToolTip(tr("Drop a material to pave this ground surface"));
  auto* surface_form = new QFormLayout(surface_group_);
  surface_form->addRow(tr("Material"), material_slot_);
  connect(material_slot_, &SlotWidget::item_dropped, this, &PropertiesPanel::push_surface_material);
  connect(material_slot_,
          &SlotWidget::engage_requested,
          this,
          &PropertiesPanel::library_category_requested);

  // Crosswalk asset editor (p3-s2): a second panel mode reached via the Library
  // (not the scene selection), so it is built here but hidden until edit_asset().
  auto* asset_form = new QFormLayout(asset_group_);
  const auto configure_asset_spin = [](QDoubleSpinBox* spin, double max, double step) {
    spin->setRange(0.0, max);
    spin->setSingleStep(step);
    spin->setDecimals(2);
    spin->setSuffix(QStringLiteral(" m"));
  };
  configure_asset_spin(asset_width_spin_, 20.0, 0.1); // walking depth
  configure_asset_spin(asset_border_spin_, 2.0, 0.05);
  configure_asset_spin(asset_dash_spin_, 5.0, 0.05); // 0 = solid
  configure_asset_spin(asset_gap_spin_, 5.0, 0.05);
  asset_width_spin_->setObjectName(QStringLiteral("asset_width_spin"));
  asset_dash_spin_->setObjectName(QStringLiteral("asset_dash_spin"));
  asset_form->addRow(tr("Depth"), asset_width_spin_);
  asset_form->addRow(tr("Border width"), asset_border_spin_);
  asset_form->addRow(tr("Dash length"), asset_dash_spin_);
  asset_form->addRow(tr("Dash gap"), asset_gap_spin_);
  asset_material_slot_->setObjectName(QStringLiteral("asset_material_slot"));
  asset_material_slot_->setToolTip(tr("Drop a material to set this crosswalk's paint"));
  asset_form->addRow(tr("Material"), asset_material_slot_);
  asset_category_edit_->setObjectName(QStringLiteral("asset_category_edit"));
  asset_form->addRow(tr("Category"), asset_category_edit_);
  asset_preview_->setObjectName(QStringLiteral("asset_preview"));
  asset_preview_->setMinimumHeight(48);
  asset_form->addRow(tr("Preview"), asset_preview_);
  asset_hint_->setWordWrap(true);
  asset_hint_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  asset_form->addRow(asset_hint_);
  asset_group_->hide();

  for (QDoubleSpinBox* spin :
       {asset_width_spin_, asset_border_spin_, asset_dash_spin_, asset_gap_spin_}) {
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { commit_asset_edit(); });
  }
  connect(asset_category_edit_, &QLineEdit::editingFinished, this, [this] { commit_asset_edit(); });
  connect(asset_material_slot_, &SlotWidget::item_dropped, this, [this](const QString& key) {
    asset_material_key_ = key;
    asset_material_slot_->set_item(key);
    commit_asset_edit();
  });
  connect(asset_material_slot_, &SlotWidget::engage_requested, this, [this] {
    emit library_category_requested(QStringLiteral("Materials"));
  });

  // Prop-set asset editor (p6-s5): a variable-length list of weighted model
  // entries, built here and hidden until edit_prop_set_asset(). Saved
  // explicitly (the Save button) rather than per-field, since add/remove
  // reshapes the form.
  auto* prop_set_layout = new QVBoxLayout(prop_set_group_);
  auto* entries_container = new QWidget(prop_set_group_);
  prop_set_entries_layout_ = new QVBoxLayout(entries_container);
  prop_set_entries_layout_->setContentsMargins(0, 0, 0, 0);
  prop_set_layout->addWidget(entries_container);
  prop_set_add_button_ = new QPushButton(tr("Add entry"), prop_set_group_);
  prop_set_add_button_->setObjectName(QStringLiteral("prop_set_add_entry"));
  prop_set_save_button_ = new QPushButton(tr("Save"), prop_set_group_);
  prop_set_save_button_->setObjectName(QStringLiteral("prop_set_save"));
  auto* prop_set_buttons = new QHBoxLayout;
  prop_set_buttons->addWidget(prop_set_add_button_);
  prop_set_buttons->addStretch();
  prop_set_buttons->addWidget(prop_set_save_button_);
  prop_set_layout->addLayout(prop_set_buttons);
  prop_set_hint_->setWordWrap(true);
  prop_set_hint_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  prop_set_layout->addWidget(prop_set_hint_);
  prop_set_group_->hide();
  connect(prop_set_add_button_, &QPushButton::clicked, this, [this] {
    // A fresh row defaults to the first bundled model at weight 1.
    const auto& ids = props::ids();
    add_prop_set_row(ids.empty() ? QString() : QString::fromStdString(ids.front()), 1.0, true);
  });
  connect(prop_set_save_button_, &QPushButton::clicked, this, [this] { commit_prop_set_edit(); });

  // Prop asset editor (p6-s11): a single Default scale spin, built here and
  // hidden until edit_prop_asset(). Commits per-field on editingFinished (the
  // crosswalk pattern) — one spin never reshapes the form, so no Save button.
  auto* prop_layout = new QFormLayout(prop_group_);
  prop_model_label_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  prop_layout->addRow(tr("Model"), prop_model_label_);
  prop_scale_spin_->setObjectName(QStringLiteral("prop_default_scale"));
  prop_scale_spin_->setRange(0.05, 20.0);
  prop_scale_spin_->setDecimals(2);
  prop_scale_spin_->setSingleStep(0.1);
  prop_layout->addRow(tr("Default scale"), prop_scale_spin_);
  prop_scale_hint_->setWordWrap(true);
  prop_scale_hint_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  prop_layout->addRow(prop_scale_hint_);
  prop_hint_->setWordWrap(true);
  prop_hint_->setStyleSheet(QStringLiteral("color: palette(mid);"));
  prop_layout->addRow(prop_hint_);
  prop_group_->hide();
  connect(prop_scale_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    commit_prop_asset_edit();
  });

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(placeholder_);
  layout->addWidget(name_row_);
  layout->addLayout(form_);
  layout->addWidget(lane_group_);
  layout->addWidget(elevation_group_);
  layout->addWidget(corner_group_);
  layout->addWidget(stopline_group_);
  layout->addWidget(surface_spans_group_);
  layout->addWidget(maneuvers_group_);
  layout->addWidget(signalization_group_);
  layout->addWidget(junction_group_);
  layout->addWidget(signal_group_);
  layout->addWidget(object_group_);
  layout->addWidget(style_group_);
  layout->addWidget(surface_group_);
  layout->addWidget(asset_group_);
  layout->addWidget(prop_set_group_);
  layout->addWidget(prop_group_);
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
    // set_lane_width refuses a lane whose width varies along s; the spin is
    // disabled in that case, but guard here too so a stray editingFinished
    // (focus-out) never reaches the kernel with a doomed command.
    if (!lane_width_is_constant(*lane)) {
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
    const auto current = active_node_height();
    if (!current.has_value() || std::abs(elevation_spin_->value() - *current) < 1e-9) {
      return;
    }
    const auto node = elevation_tool_->active_node();
    push(edit::set_node_elevation(
        document_.network(), node->first, node->second, elevation_spin_->value()));
  });

  // Commit the active corner's radius on focus-out, skipping the push when the
  // value is unchanged — the same re-entrancy guard every other editor here
  // uses, so refresh_corner() re-seeding the spin box never echoes a command.
  connect(corner_radius_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    const std::optional<JunctionCornerInfo> info = active_corner_info();
    if (!info.has_value() || std::abs(corner_radius_spin_->value() - info->radius) < 1e-9) {
      return;
    }
    const auto corner = corner_tool_->active_corner();
    push(edit::set_corner_radius(document_.network(),
                                 corner->junction,
                                 corner->arm_a,
                                 corner->arm_b,
                                 corner_radius_spin_->value()));
  });

  // The active stop line's setback commits on focus-out, with the same
  // unchanged-value guard so refresh_stopline() re-seeding never echoes a
  // command back.
  connect(stopline_distance_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    const std::optional<JunctionStopLineInfo> info = active_stopline_info();
    if (!info.has_value() || std::abs(stopline_distance_spin_->value() - info->distance) < 1e-9) {
      return;
    }
    const auto line = stopline_tool_->active_stopline();
    push(edit::set_stopline_distance(
        document_.network(), line->junction, line->arm, stopline_distance_spin_->value()));
  });

  // The junction-wide default commits on focus-out too, with the same skip
  // guard. Typing 0 (the "Derived" position) is the CLEAR gesture — but only
  // when a default is actually set: clearing nothing is a kernel error.
  // The lock toggle is one command per click. The refresh path blocks this
  // signal while seeding, so a programmatic state change never echoes back.
  connect(junction_locked_check_, &QCheckBox::toggled, this, [this](bool locked) {
    const JunctionId id = selection_.primary().junction;
    const Junction* junction = document_.network().junction(id);
    if (junction == nullptr || junction->locked == locked) {
      return; // set_junction_locked refuses a no-op
    }
    push(edit::set_junction_locked(document_.network(), id, locked));
  });

  connect(junction_radius_spin_, &QDoubleSpinBox::editingFinished, this, [this] {
    const JunctionId id = selection_.primary().junction;
    const Junction* junction = document_.network().junction(id);
    if (junction == nullptr) {
      return;
    }
    const double value = junction_radius_spin_->value();
    const bool has_default = junction->default_corner_radius.has_value();
    if (value < kCornerRadiusMin) {
      if (!has_default) {
        return; // already derived — nothing to clear
      }
      push(edit::set_junction_default_corner_radius(document_.network(), id, 0.0));
      return;
    }
    if (has_default && std::abs(*junction->default_corner_radius - value) < 1e-9) {
      return;
    }
    push(edit::set_junction_default_corner_radius(document_.network(), id, value));
  });

  // Signal pose: commit s/t/heading on focus-out through move_signal, with the
  // same unchanged-value skip guard so refresh() after undo never re-commits.
  for (QDoubleSpinBox* spin : {signal_s_spin_, signal_t_spin_, signal_h_spin_}) {
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { push_signal_move(); });
  }

  // A scene selection change leaves the crosswalk asset editor (dual-mode
  // flag), then rebuilds the normal view.
  connect(&selection_, &SelectionModel::selection_changed, this, [this] {
    asset_mode_ = false;
    prop_set_mode_ = false;
    prop_mode_ = false;
    refresh();
  });
  connect(&document_, &Document::loaded, this, &PropertiesPanel::refresh);
  // Commands and undo/redo change lane values without touching the
  // selection — re-sync the editors from the network.
  connect(&document_, &Document::mesh_changed, this, &PropertiesPanel::refresh);
  refresh();
}

void PropertiesPanel::refresh() {
  // The crosswalk asset editor owns the panel until the selection changes; a
  // propagate command's mesh_changed must not tear it down mid-edit.
  if (asset_mode_) {
    return;
  }
  clear_rows();

  // The Corner and Stop line sections follow their tool's sub-selection, not
  // the row layout below, and every path through this function (road, lane,
  // junction, empty) is covered by their own predicates — so decide them once,
  // up front.
  refresh_corner();
  refresh_stopline();
  refresh_surface_spans();
  refresh_maneuvers();
  refresh_signalization();

  // The asset editors are Library-driven modes; any scene selection closes them.
  asset_group_->hide();
  prop_set_group_->hide();
  prop_group_->hide();
  // Shown only on the road path below; every early return leaves it hidden.
  style_group_->hide();
  // Shown only on the ground-surface path below; hidden everywhere else.
  surface_group_->hide();
  // Shown only on the junction path below (p4-s2); hidden everywhere else.
  junction_group_->hide();

  // The primary entry (most recently selected) drives the panel.
  const SelectionEntry primary = selection_.primary();

  // A signal entry carries its owning road, so check it BEFORE the road path:
  // a selected signal shows its own pose section, not the road under it.
  if (const Signal* signal = document_.network().signal(primary.signal)) {
    name_row_->hide();
    lane_group_->hide();
    elevation_group_->hide();
    object_group_->hide();
    placeholder_->hide();
    refresh_signal(*signal);
    return;
  }
  signal_group_->hide();

  // Same for a prop: its entry carries the owning road, but the selection is
  // the object, so show the object's own fields rather than that road's.
  if (const Object* object = document_.network().object(primary.object)) {
    name_row_->hide();
    lane_group_->hide();
    elevation_group_->hide();
    placeholder_->hide();
    refresh_object(*object);
    return;
  }
  object_group_->hide();

  const Road* road = document_.network().road(primary.road);
  if (road == nullptr) {
    name_row_->hide();
    lane_group_->hide();
    elevation_group_->hide();
    // A selected ground surface (#215) has no road but is a real entity — show
    // its read-only stats. It is auto-managed by the enclosing road loop, so
    // there is nothing to edit here. Placed before the road fallback.
    if (const Surface* surface = document_.network().surface(primary.surface)) {
      placeholder_->hide();
      add_row(tr("Type"), tr("Ground surface"));
      double area = 0.0;
      for (const SurfaceMesh& sm : document_.mesh().surfaces) {
        if (sm.surface == primary.surface) {
          area = mesh_area(sm.mesh);
          break;
        }
      }
      add_row(tr("Area"), tr("%1 m²").arg(area, 0, 'f', 1));
      add_row(tr("Bounding roads"), QString::number(surface->bounding_roads.size()));
      // The Materials slot reflects the surface's stored material (empty →
      // placeholder = default grass).
      material_slot_->set_item(QString::fromStdString(surface->material));
      surface_group_->show();
      return;
    }
    // A selected junction floor has no road but is a real entity — show its
    // topology instead of the empty placeholder (gate finding 4).
    if (const Junction* junction = document_.network().junction(primary.junction)) {
      placeholder_->hide();
      add_row(tr("OpenDRIVE id"), tr("junction %1").arg(QString::fromStdString(junction->odr_id)));
      add_row(tr("Arms"), QString::number(junction->arms.size()));
      add_row(tr("Connections"), QString::number(junction->connections.size()));
      refresh_junction(*junction);
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

  // The road-style slot is write-only: a road stores no style identity, so it
  // shows a placeholder rather than reflecting a value.
  style_slot_->set_item(QString());
  style_group_->show();
}

// --- scrub-editing -----------------------------------------------------------

ScrubLabel* PropertiesPanel::install_scrub(ScrubLabel* label, ScrubBinding binding) {
  const std::size_t index = scrubs_.size();
  scrubs_.push_back(std::move(binding));
  connect(label, &ScrubLabel::scrub_started, this, [this, index] { begin_scrub(index); });
  connect(label, &ScrubLabel::scrub_moved, this, &PropertiesPanel::update_scrub);
  connect(label, &ScrubLabel::scrub_finished, this, &PropertiesPanel::finish_scrub);
  connect(label, &ScrubLabel::scrub_cancelled, this, &PropertiesPanel::cancel_scrub);
  return label;
}

void PropertiesPanel::begin_scrub(std::size_t index) {
  cancel_scrub(); // a still-open session (lost release) must not leak into this one

  const ScrubBinding& binding = scrubs_[index];
  const std::optional<double> baseline = binding.baseline();
  if (!baseline.has_value()) {
    return; // nothing editable under this label right now — the drag is inert
  }
  // Open the session on the attribute's CURRENT value: applying it is a no-op,
  // which gives update_preview a base state to rebuild against without the
  // gesture having changed anything yet.
  if (!document_.begin_preview(binding.factory(document_.network(), *baseline))) {
    return;
  }
  scrub_active_ = index;
  scrub_baseline_ = *baseline;
  scrub_value_ = *baseline;
}

void PropertiesPanel::update_scrub(double delta) {
  if (!scrub_active_.has_value()) {
    return;
  }
  const ScrubBinding& binding = scrubs_[*scrub_active_];
  // The spin box's range is the attribute's range — scrubbing must not reach
  // values typing cannot.
  const double value = std::clamp(scrub_baseline_ + (delta * binding.units_per_pixel),
                                  binding.spin->minimum(),
                                  binding.spin->maximum());
  if (!document_.update_preview([&binding, value](const RoadNetwork& network) {
        return binding.factory(network, value);
      })) {
    return; // the session stays at its last good state (Document's contract)
  }
  scrub_value_ = value;
  const QSignalBlocker blocker(binding.spin);
  binding.spin->setValue(value); // live readout of what the drag is doing
}

void PropertiesPanel::finish_scrub() {
  if (!scrub_active_.has_value()) {
    return;
  }
  const ScrubBinding& binding = scrubs_[*scrub_active_];
  scrub_active_.reset();
  // A gesture that ended back where it started is not an edit: cancelling
  // instead of committing keeps a no-op entry off the undo stack.
  if (std::abs(scrub_value_ - scrub_baseline_) < 1e-9) {
    document_.cancel_preview();
  } else {
    document_.commit_preview();
    emit status_message(tr("%1: %2").arg(binding.spin->objectName()).arg(scrub_value_));
  }
  refresh();
}

void PropertiesPanel::cancel_scrub() {
  if (!scrub_active_.has_value()) {
    return;
  }
  scrub_active_.reset();
  document_.cancel_preview(); // byte-identical restore of the pre-scrub state
  refresh();
}

std::optional<double> PropertiesPanel::active_node_height() const {
  if (elevation_tool_ == nullptr) {
    return std::nullopt;
  }
  const auto node = elevation_tool_->active_node();
  if (!node.has_value()) {
    return std::nullopt;
  }
  const Road* road = document_.network().road(node->first);
  if (road == nullptr) {
    return std::nullopt;
  }
  const auto stations = edit::waypoint_stations(*road);
  if (!stations.has_value() || node->second >= stations->size()) {
    return std::nullopt;
  }
  return eval_profile(road->elevation, (*stations)[node->second]);
}

// --- prop --------------------------------------------------------------------

void PropertiesPanel::refresh_object(const Object& object) {
  add_row(tr("OpenDRIVE id"), tr("object %1").arg(QString::fromStdString(object.odr_id)));
  // This line says which KIND of object it is, from the OpenDRIVE @type (§13.1).
  object_kind_label_->setText(object_type_name(object.type));
  add_row(tr("Position"), tr("s %1 m, t %2 m").arg(object.s, 0, 'f', 2).arg(object.t, 0, 'f', 2));

  // A marking instance (crosswalk / stencil / marking-curve) exposes the paint
  // Material override slot; every other object (a prop) exposes the Model slot.
  // The two share the object group — toggle the rows and the title per kind.
  const std::optional<std::string> marking_material =
      object.crosswalk.has_value()       ? std::optional(object.crosswalk->material)
      : object.stencil.has_value()       ? std::optional(object.stencil->material)
      : object.marking_curve.has_value() ? std::optional(object.marking_curve->material)
                                         : std::nullopt;
  const bool is_marking = marking_material.has_value();
  object_form_->setRowVisible(model_slot_, !is_marking);
  object_form_->setRowVisible(instance_material_slot_, is_marking);
  if (is_marking) {
    instance_material_slot_->set_item(QString::fromStdString(*marking_material));
    object_group_->setTitle(tr("Marking"));
  } else {
    // Object::name IS the prop model id the mesher renders (mesh.hpp
    // ObjectInstance::model_id) — that is what the slot shows and replaces.
    model_slot_->set_item(QString::fromStdString(object.name));
    object_group_->setTitle(tr("Prop"));
  }
  // Size is meaningful only for a prop that actually renders a bundled model:
  // a marking has no model, and an unresolvable @name has no height to scale.
  const bool resizable = !is_marking && props::model(object.name) != nullptr;
  object_form_->setRowVisible(object_height_spin_, resizable);
  if (resizable) {
    if (const std::optional<double> height = primary_prop_effective_height(document_.network());
        height.has_value()) {
      const QSignalBlocker blocker(object_height_spin_);
      object_height_spin_->setValue(*height);
    }
  }
  object_group_->show();
}

std::optional<double>
PropertiesPanel::primary_prop_effective_height(const RoadNetwork& network) const {
  const Object* object = network.object(selection_.primary().object);
  if (object == nullptr) {
    return std::nullopt;
  }
  const props::PropModel* model = props::model(object->name);
  if (model == nullptr) {
    return std::nullopt; // a marking or a model this build does not know
  }
  if (object->height.has_value() && *object->height > 0.0) {
    return *object->height;
  }
  return model->height;
}

std::unique_ptr<edit::Command> PropertiesPanel::resize_selected_props(const RoadNetwork& network,
                                                                      double value,
                                                                      bool absolute) const {
  // The primary's height sets the batch factor, so every prop keeps its
  // relative size. Read from `network` — see the header's note on baselines.
  const Object* primary = network.object(selection_.primary().object);
  const props::PropModel* primary_model =
      primary == nullptr ? nullptr : props::model(primary->name);
  if (primary_model == nullptr) {
    return nullptr;
  }
  const double primary_height = primary->height.has_value() && *primary->height > 0.0
                                    ? *primary->height
                                    : primary_model->height;
  if (!(primary_height > 0.0)) {
    return nullptr;
  }

  std::vector<std::pair<ObjectId, Object>> updates;
  std::vector<ObjectId> seen;
  for (const ObjectId id : selection_.selected_objects()) {
    if (std::ranges::find(seen, id) != seen.end()) {
      continue;
    }
    seen.push_back(id);
    const Object* object = network.object(id);
    if (object == nullptr) {
      continue;
    }
    const props::PropModel* model = props::model(object->name);
    if (model == nullptr) {
      continue; // markings and unknown models are not resizable
    }
    const double height =
        object->height.has_value() && *object->height > 0.0 ? *object->height : model->height;
    if (!(height > 0.0)) {
      continue;
    }
    const double factor = absolute ? value / height : value / primary_height;
    Object updated = *object;
    updated.height = absolute ? value : height * factor;
    // Siblings scale only if the object already declares them — resizing must
    // never invent an optional OpenDRIVE attribute.
    if (updated.radius.has_value()) {
      updated.radius = *updated.radius * factor;
    }
    if (updated.width.has_value()) {
      updated.width = *updated.width * factor;
    }
    if (updated.length.has_value()) {
      updated.length = *updated.length * factor;
    }
    updates.emplace_back(id, std::move(updated));
  }
  if (updates.empty()) {
    return nullptr;
  }
  return edit::update_objects(network, std::move(updates), "Resize Props");
}

void PropertiesPanel::push_object_model(const QString& key) {
  const std::vector<ObjectId> objects = selection_.selected_objects();
  if (objects.empty() || key.isEmpty()) {
    return;
  }
  push(edit::set_object_model(document_.network(), objects.back(), key.toStdString()));
}

void PropertiesPanel::push_road_style(const QString& key) {
  const std::vector<RoadId> roads = selection_.selected_roads();
  if (roads.empty() || key.isEmpty()) {
    return;
  }
  // The slot emits the library item key ("style.urban"); style_for maps both the
  // item-key and the style-name vocabularies to a RoadStyle (like the prop slot,
  // this passes the dropped key straight through — no library lookup here).
  push(edit::apply_road_style(document_.network(), roads.back(), style_for(key)));
}

namespace {

/// The canonical material name for a dropped Materials library item, or nullopt
/// for a key this build does not recognise. Delegates to the MaterialCatalog so
/// every catalog spelling — "material.asphalt", "asphalt", "rm:asphalt" — and
/// every material (asphalt_worn included) resolves without a hardcoded list;
/// returns an optional so an unknown key is rejected with a toast, not a default.
[[nodiscard]] std::optional<std::string> material_for_key(const QString& key) {
  static const MaterialCatalog catalog;
  if (const MaterialDef* def = catalog.find_material(key.toStdString()); def != nullptr) {
    return def->name;
  }
  return std::nullopt;
}

/// The RoadMark a dropped Markings library `key` authors, or nullopt for a key
/// this build does not know. Resolves the key against the bundled manifest
/// (`:/library/manifest.json`, parsed once) and reuses mark_from_item — the same
/// item→mark path the viewport drop takes, so the slot and the drop never
/// disagree. Like the material slot it consults only the built-in catalogue; a
/// project overlay defines no markings in this build.
[[nodiscard]] std::optional<RoadMark> mark_for_key(const QString& key) {
  static const std::optional<LibraryManifest> manifest = []() -> std::optional<LibraryManifest> {
    QFile file(QStringLiteral(":/library/manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
      return std::nullopt;
    }
    auto parsed = LibraryManifest::parse(file.readAll());
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    return std::move(*parsed);
  }();
  if (!manifest.has_value()) {
    return std::nullopt;
  }
  for (const LibraryItem& item : manifest->items()) {
    if (item.kind == LibraryItem::Kind::Marking && item.key == key) {
      return mark_from_item(item);
    }
  }
  return std::nullopt;
}

} // namespace

void PropertiesPanel::push_surface_material(const QString& key) {
  const std::vector<SurfaceId> surfaces = selection_.selected_surfaces();
  if (surfaces.empty() || key.isEmpty()) {
    return;
  }
  const std::optional<std::string> material = material_for_key(key);
  if (!material.has_value()) {
    // Unknown material key: hint rather than silently applying a default (the
    // style slot's silent fallback is wrong for a slot that reflects a value).
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  const Surface* surface = document_.network().surface(surfaces.back());
  if (surface != nullptr && surface->material == *material) {
    return; // no change — a drop that changes nothing pushes no command
  }
  push(edit::set_surface_material(document_.network(), surfaces.back(), *material));
}

void PropertiesPanel::push_instance_material(const QString& key) {
  const std::vector<ObjectId> objects = selection_.selected_objects();
  if (objects.empty() || key.isEmpty()) {
    return;
  }
  // Validate against the catalog (reject an unknown key with a hint, no silent
  // default), but store the dropped library key form so it matches the asset's
  // Default Material spelling and round-trips verbatim.
  if (!material_for_key(key).has_value()) {
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  const ObjectId id = objects.back();
  const Object* object = document_.network().object(id);
  if (object == nullptr) {
    return;
  }
  const std::string code = key.toStdString();
  Object updated = *object;
  // Pin the paint on THIS instance (material_override), so a later asset
  // Default-Material change skips it (materialize_* honors the flag).
  if (updated.crosswalk.has_value()) {
    if (updated.crosswalk->material == code && updated.crosswalk->material_override) {
      return; // no change
    }
    updated.crosswalk->material = code;
    updated.crosswalk->material_override = true;
  } else if (updated.stencil.has_value()) {
    if (updated.stencil->material == code && updated.stencil->material_override) {
      return;
    }
    updated.stencil->material = code;
    updated.stencil->material_override = true;
  } else if (updated.marking_curve.has_value()) {
    if (updated.marking_curve->material == code && updated.marking_curve->material_override) {
      return;
    }
    updated.marking_curve->material = code;
    updated.marking_curve->material_override = true;
  } else {
    emit status_message(tr("This object has no marking material to override"));
    return;
  }
  push(edit::update_objects(
      document_.network(), {{id, std::move(updated)}}, "Override Marking Material"));
  emit status_message(tr("Overrode this marking's material"));
}

void PropertiesPanel::push_lane_material(const QString& key) {
  const Lane* lane = primary_lane();
  if (lane == nullptr || key.isEmpty()) {
    return;
  }
  if (lane->odr_id == 0) {
    emit status_message(tr("The centre lane can't carry a material"));
    return;
  }
  const std::optional<std::string> material = material_for_key(key);
  if (!material.has_value()) {
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  static const MaterialCatalog catalog;
  const MaterialDef* def = catalog.find_material(*material);
  const double friction = def != nullptr ? def->friction : 0.9;
  // A slot/drop authors ONE constant record covering the lane (the first-record
  // simplification set_road_mark uses); a multi-record profile comes from the
  // kernel API. surface = "rm:<name>" marks it as RoadMaker-authored.
  const LaneMaterial record{.s_offset = 0.0, .friction = friction, .surface = "rm:" + *material};
  if (lane->materials.size() == 1 && lane->materials.front() == record) {
    return; // no change — a drop that changes nothing pushes no command
  }
  push(edit::set_lane_material(document_.network(), selection_.primary().lane, {record}));
}

void PropertiesPanel::push_corner_sidewalk_material(const QString& key) {
  const std::optional<JunctionCornerInfo> info = active_corner_info();
  if (!info.has_value() || key.isEmpty()) {
    return;
  }
  const std::optional<std::string> material = material_for_key(key);
  if (!material.has_value()) {
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  if (info->sidewalk_material == *material) {
    return; // no change — a drop that changes nothing pushes no command
  }
  const auto corner = corner_tool_->active_corner();
  push(edit::set_corner_sidewalk_material(
      document_.network(), corner->junction, corner->arm_a, corner->arm_b, *material));
}

void PropertiesPanel::push_corner_median_material(const QString& key) {
  const std::optional<JunctionCornerInfo> info = active_corner_info();
  if (!info.has_value() || key.isEmpty()) {
    return;
  }
  const std::optional<std::string> material = material_for_key(key);
  if (!material.has_value()) {
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  if (info->median_material == *material) {
    return;
  }
  const auto corner = corner_tool_->active_corner();
  push(edit::set_corner_median_material(
      document_.network(), corner->junction, corner->arm_a, corner->arm_b, *material));
}

void PropertiesPanel::push_junction_material(const QString& key) {
  const JunctionId id = selection_.primary().junction;
  const Junction* junction = document_.network().junction(id);
  if (junction == nullptr || key.isEmpty()) {
    return;
  }
  const std::optional<std::string> material = material_for_key(key);
  if (!material.has_value()) {
    emit status_message(tr("“%1” isn't a known material").arg(key));
    return;
  }
  if (junction->material == *material) {
    return;
  }
  push(edit::set_junction_material(document_.network(), id, *material));
}

void PropertiesPanel::push_marking(const QString& key) {
  const Lane* lane = primary_lane();
  if (lane == nullptr || key.isEmpty()) {
    return;
  }
  const std::optional<RoadMark> mark = mark_for_key(key);
  if (!mark.has_value()) {
    emit status_message(tr("“%1” isn't a known marking").arg(key));
    return;
  }
  // A drop that changes nothing pushes no command (mirrors the material slot).
  const RoadMark current = lane->road_marks.empty() ? RoadMark{} : lane->road_marks.front();
  if (!lane->road_marks.empty() && current.type == mark->type && current.color == mark->color &&
      std::abs(current.width - mark->width) < 1e-9) {
    return;
  }
  push(edit::set_road_mark(document_.network(), selection_.primary().lane, *mark));
}

void PropertiesPanel::refresh_signal(const Signal& signal) {
  add_row(tr("OpenDRIVE id"), tr("signal %1").arg(QString::fromStdString(signal.odr_id)));
  const bool dynamic = signal.dynamic.value_or(false);
  signal_kind_label_->setText(dynamic ? tr("Dynamic signal (traffic light)")
                                      : tr("Static signal (sign)"));
  add_row(tr("Type / subtype"),
          tr("%1 / %2")
              .arg(QString::fromStdString(signal.type.empty() ? "—" : signal.type))
              .arg(QString::fromStdString(signal.subtype.empty() ? "—" : signal.subtype)));
  if (!signal.country.empty()) {
    add_row(tr("Country"), QString::fromStdString(signal.country));
  }
  // Programmatic sync must not echo a move_signal back — block the editingFinished
  // guard's siblings while we set values.
  const QSignalBlocker block_s(signal_s_spin_);
  const QSignalBlocker block_t(signal_t_spin_);
  const QSignalBlocker block_h(signal_h_spin_);
  signal_s_spin_->setValue(signal.s);
  signal_t_spin_->setValue(signal.t);
  signal_h_spin_->setValue(signal.h_offset);
  // @text is legal on any signal but only meaningful on a static sign; disable
  // the editor for a dynamic head. Seeded under a blocker so the re-seed never
  // commits back.
  {
    const QSignalBlocker block_text(signal_text_edit_);
    signal_text_edit_->setPlainText(QString::fromStdString(signal.text));
  }
  signal_text_edit_->setEnabled(!dynamic);
  signal_group_->show();
}

void PropertiesPanel::push_signal_text() {
  const std::vector<SignalId> signal_ids = selection_.selected_signals();
  if (signal_ids.empty()) {
    return;
  }
  const Signal* signal = document_.network().signal(signal_ids.back());
  if (signal == nullptr) {
    return;
  }
  const std::string text = signal_text_edit_->toPlainText().toStdString();
  if (text == signal->text) {
    return; // no-op: the re-entrancy guard (a refresh re-seed must not commit)
  }
  push(edit::set_signal_text(document_.network(), signal_ids.back(), text));
}

bool PropertiesPanel::eventFilter(QObject* watched, QEvent* event) {
  if (watched == signal_text_edit_) {
    if (event->type() == QEvent::FocusOut) {
      push_signal_text();
    } else if (event->type() == QEvent::KeyPress) {
      auto* key = static_cast<QKeyEvent*>(event);
      if (key->key() == Qt::Key_Escape) {
        // Restore the committed value and drop focus (no command).
        const std::vector<SignalId> ids = selection_.selected_signals();
        const Signal* signal = ids.empty() ? nullptr : document_.network().signal(ids.back());
        const QSignalBlocker block_text(signal_text_edit_);
        signal_text_edit_->setPlainText(signal == nullptr ? QString{}
                                                          : QString::fromStdString(signal->text));
        signal_text_edit_->clearFocus();
        return true;
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void PropertiesPanel::push_signal_move() {
  const std::vector<SignalId> signal_ids = selection_.selected_signals();
  if (signal_ids.empty()) {
    return;
  }
  const Signal* signal = document_.network().signal(signal_ids.back());
  if (signal == nullptr) {
    return;
  }
  // Skip the push when nothing changed — the re-entrancy guard that keeps a
  // refresh()-driven setValue from committing a redundant command.
  if (std::abs(signal->s - signal_s_spin_->value()) < 1e-9 &&
      std::abs(signal->t - signal_t_spin_->value()) < 1e-9 &&
      std::abs(signal->h_offset - signal_h_spin_->value()) < 1e-9) {
    return;
  }
  push(edit::move_signal(document_.network(),
                         signal_ids.back(),
                         signal_s_spin_->value(),
                         signal_t_spin_->value(),
                         signal_h_spin_->value()));
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

void PropertiesPanel::set_stopline_tool(StopLineTool* tool) {
  stopline_tool_ = tool;
  if (stopline_tool_ != nullptr) {
    connect(stopline_tool_,
            &StopLineTool::stopline_selection_changed,
            this,
            &PropertiesPanel::refresh_stopline);
  }
  refresh_stopline();
}

void PropertiesPanel::set_junction_surface_tool(JunctionSurfaceTool* tool) {
  junction_surface_tool_ = tool;
  if (junction_surface_tool_ != nullptr) {
    connect(junction_surface_tool_,
            &JunctionSurfaceTool::surface_span_selection_changed,
            this,
            &PropertiesPanel::refresh_surface_spans);
  }
  refresh_surface_spans();
}

void PropertiesPanel::refresh_surface_spans() {
  // Rows are rebuilt wholesale: they are cheap, and rebuilding is what keeps
  // the display honest after a regeneration changed the turn set.
  // deleteLater for the same reason refresh_maneuvers uses it: a row editor
  // pushes from inside its own signal and the re-mesh lands back here, so a
  // plain delete would destroy a widget that is still emitting.
  while (QLayoutItem* item = surface_spans_layout_->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      row->setParent(nullptr);
      row->deleteLater();
    }
    delete item;
  }

  const JunctionId junction = selection_.primary().junction;
  const std::vector<JunctionSurfaceSpanInfo> spans =
      junction.is_valid() ? junction_surface_spans(document_.network(), junction)
                          : std::vector<JunctionSurfaceSpanInfo>{};
  if (spans.empty()) {
    // Empty for a stale id and for a junction with no floor to control — a
    // span (virtual) junction has no connections at all.
    surface_spans_group_->hide();
    return;
  }

  const std::optional<ActiveSurfaceSpan> active =
      junction_surface_tool_ != nullptr ? junction_surface_tool_->active_span() : std::nullopt;

  for (const JunctionSurfaceSpanInfo& info : spans) {
    auto* row = new QWidget(surface_spans_group_);
    row->setObjectName(QStringLiteral("surface_span_row"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);

    // A flat button rather than a label: clicking the name is what selects the
    // span in the tool, so the viewport and the panel always agree about which
    // one is under attention.
    auto* label = new QPushButton(tr("Turn %1").arg(QString::fromStdString(info.road_odr_id)), row);
    label->setObjectName(QStringLiteral("surface_span_label"));
    label->setFlat(true);
    label->setCursor(Qt::PointingHandCursor);
    if (active.has_value() && active->road == info.road) {
      QFont bold = label->font();
      bold.setBold(true);
      label->setFont(bold);
    }
    row_layout->addWidget(label, 1);

    auto* samples = new QCheckBox(tr("Samples"), row);
    samples->setObjectName(QStringLiteral("surface_span_samples_check"));
    samples->setToolTip(
        tr("Let this turn's samples shape the floor's elevation and triangulation. Clearing it "
           "leaves the footprint in the union, so the pavement's extent never changes."));
    {
      // Programmatic seeding must not echo a toggle back as a command — the
      // same guard every other editor here uses.
      const QSignalBlocker blocker(samples);
      samples->setChecked(info.included);
    }
    const RoadId road = info.road;
    const bool included = info.included;
    connect(samples, &QCheckBox::toggled, this, [this, junction, road, included](bool checked) {
      if (checked == included) {
        return; // a refresh re-seed, not a click
      }
      push(edit::set_surface_span_included(document_.network(), junction, road, checked));
    });
    row_layout->addWidget(samples);

    auto* sort_label = new QLabel(QString::number(info.sort_index), row);
    sort_label->setObjectName(QStringLiteral("surface_span_sort_label"));
    sort_label->setToolTip(tr("Precedence where this turn's footprint overlaps another's — "
                              "higher wins."));
    sort_label->setMinimumWidth(24);
    sort_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row_layout->addWidget(sort_label);

    const auto nudge = [this, junction, road](int delta, const JunctionSurfaceSpanInfo& span) {
      push(edit::set_surface_span_sort_index(
          document_.network(), junction, road, span.sort_index + delta));
    };
    const JunctionSurfaceSpanInfo snapshot = info;
    auto* lower = new QToolButton(row);
    lower->setObjectName(QStringLiteral("surface_span_lower_button"));
    lower->setText(QStringLiteral("↓"));
    lower->setToolTip(tr("Lower this span: it loses overlaps to the spans above it"));
    connect(lower, &QToolButton::clicked, this, [nudge, snapshot] { nudge(-1, snapshot); });
    row_layout->addWidget(lower);

    auto* raise = new QToolButton(row);
    raise->setObjectName(QStringLiteral("surface_span_raise_button"));
    raise->setText(QStringLiteral("↑"));
    raise->setToolTip(tr("Raise this span: it wins overlaps against the spans below it"));
    connect(raise, &QToolButton::clicked, this, [nudge, snapshot] { nudge(+1, snapshot); });
    row_layout->addWidget(raise);

    connect(label, &QPushButton::clicked, this, [this, road] {
      if (junction_surface_tool_ != nullptr) {
        junction_surface_tool_->select_span(road);
      }
    });
    surface_spans_layout_->addWidget(row);
  }
  surface_spans_group_->show();
}

void PropertiesPanel::set_maneuver_tool(ManeuverTool* tool) {
  maneuver_tool_ = tool;
  if (maneuver_tool_ != nullptr) {
    connect(maneuver_tool_,
            &ManeuverTool::maneuver_selection_changed,
            this,
            &PropertiesPanel::refresh_maneuvers);
  }
  refresh_maneuvers();
}

void PropertiesPanel::refresh_maneuvers() {
  // Rows are rebuilt wholesale: they are cheap, and rebuilding is what keeps
  // the display honest after a regeneration changed the turn set.
  //
  // deleteLater, not delete: a row editor pushes its command from inside its own
  // signal, the command re-meshes, and the re-mesh lands right back here — a
  // plain delete would destroy the widget still emitting. Re-parenting to
  // nothing first is what keeps findChild (and the next rebuild) from seeing the
  // corpses in the meantime.
  while (QLayoutItem* item = maneuvers_layout_->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      row->setParent(nullptr);
      row->deleteLater();
    }
    delete item;
  }

  const RoadNetwork& network = document_.network();
  const JunctionId junction = selection_.primary().junction;
  const std::vector<JunctionManeuverInfo> all = junction.is_valid()
                                                    ? junction_maneuvers(network, junction)
                                                    : std::vector<JunctionManeuverInfo>{};
  if (all.empty()) {
    // Empty for a stale id and for a junction with no connections at all — a
    // span (virtual) junction has no turns to author.
    maneuvers_group_->hide();
    return;
  }

  // A FOREIGN junction (read from someone else's file) carries no arms, so
  // there is nothing to replan from: its maneuvers stay readable but neither
  // Reset nor Rebuild can run. Same rule the kernel's factories enforce.
  const Junction* junction_ptr = network.junction(junction);
  const bool arm_based =
      junction_ptr != nullptr && !junction_ptr->arms.empty() && junction_ptr->spans.empty();

  const std::optional<ActiveManeuver> active =
      maneuver_tool_ != nullptr ? maneuver_tool_->active_maneuver() : std::nullopt;

  const auto road_name = [&network](RoadId road) {
    const Road* ptr = network.road(road);
    return ptr != nullptr ? QString::fromStdString(ptr->odr_id) : tr("?");
  };

  bool anything_geometric = false;
  for (const JunctionManeuverInfo& info : all) {
    anything_geometric = anything_geometric || info.locked || !info.control_points.empty() ||
                         info.start_offset != 0.0 || info.end_offset != 0.0;

    auto* row = new QWidget(maneuvers_group_);
    row->setObjectName(QStringLiteral("maneuver_row"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);

    // A flat button rather than a label: clicking the name selects the maneuver
    // in the tool, so the viewport and the panel always agree.
    auto* label = new QPushButton(tr("Turn %1  %2 → %3")
                                      .arg(QString::fromStdString(info.road_odr_id),
                                           road_name(info.from.road),
                                           road_name(info.to.road)),
                                  row);
    label->setObjectName(QStringLiteral("maneuver_label"));
    label->setFlat(true);
    label->setCursor(Qt::PointingHandCursor);
    if (active.has_value() && active->road == info.road) {
      QFont bold = label->font();
      bold.setBold(true);
      label->setFont(bold);
    }
    row_layout->addWidget(label, 1);

    const RoadId road = info.road;

    // Turn type. Index 0 is Auto (no override); the rest follow TurnType's own
    // order, so index == int(type) + 1.
    auto* turn = new QComboBox(row);
    turn->setObjectName(QStringLiteral("maneuver_turn_combo"));
    turn->addItem(tr("Auto"));
    turn->addItem(tr("Left"));
    turn->addItem(tr("Straight"));
    turn->addItem(tr("Right"));
    turn->addItem(tr("U-Turn"));
    turn->setToolTip(tr("How this turn is labelled. OpenDRIVE has no turn-type element (§12.2 "
                        "Table 56), so Auto derives it from the arm-face headings."));
    {
      const QSignalBlocker blocker(turn);
      turn->setCurrentIndex(info.overridden ? static_cast<int>(info.effective) + 1 : 0);
    }
    connect(turn, &QComboBox::currentIndexChanged, this, [this, junction, road](int index) {
      // The baseline is re-read from the LIVE network, never from a value
      // captured when the row was built: a refresh re-seed must not echo a
      // command back, and the kernel refuses a no-op outright.
      const std::optional<JunctionManeuverInfo> current =
          maneuver_info(document_.network(), junction, road);
      if (!current.has_value()) {
        return;
      }
      const std::optional<TurnType> requested =
          index <= 0 ? std::nullopt : std::optional<TurnType>(static_cast<TurnType>(index - 1));
      if (!requested.has_value() && !current->overridden) {
        return; // clearing an override that does not exist
      }
      if (requested.has_value() && current->overridden && current->effective == *requested) {
        return; // already pinned to exactly this
      }
      if (requested.has_value() && !current->overridden && current->computed == *requested) {
        return; // storing the computed type would author nothing
      }
      push(edit::set_maneuver_turn_type(document_.network(), junction, road, requested));
    });
    row_layout->addWidget(turn);

    auto* lock = new QCheckBox(tr("Lock"), row);
    lock->setObjectName(QStringLiteral("maneuver_lock_check"));
    lock->setToolTip(tr("Keep this turn's hand-shaped geometry through an explicit re-derive. Set "
                        "automatically by any path edit."));
    {
      const QSignalBlocker blocker(lock);
      lock->setChecked(info.locked);
    }
    connect(lock, &QCheckBox::toggled, this, [this, junction, road](bool checked) {
      const std::optional<JunctionManeuverInfo> current =
          maneuver_info(document_.network(), junction, road);
      if (!current.has_value() || current->locked == checked) {
        return; // a refresh re-seed, not a click
      }
      push(edit::set_maneuver_locked(document_.network(), junction, road, checked));
    });
    row_layout->addWidget(lock);

    auto* reset = new QToolButton(row);
    reset->setObjectName(QStringLiteral("maneuver_reset_button"));
    reset->setText(tr("Reset"));
    // An EXPLICIT U-turn has no derived geometry to fall back on — the planner
    // never emits one — so it can only be deleted, never reset.
    reset->setEnabled(arm_based && info.authored && !info.is_uturn_explicit);
    reset->setToolTip(info.is_uturn_explicit
                          ? tr("An explicit U-turn has no derived path to return to — delete its "
                               "connecting road instead")
                          : tr("Drop everything authored on this turn and replan it from the "
                               "junction's arms"));
    connect(reset, &QToolButton::clicked, this, [this, junction, road] {
      push(edit::reset_maneuver(document_.network(), junction, road));
    });
    row_layout->addWidget(reset);

    connect(label, &QPushButton::clicked, this, [this, road] {
      if (maneuver_tool_ != nullptr) {
        maneuver_tool_->select_maneuver(road);
      }
    });
    maneuvers_layout_->addWidget(row);
  }

  // Rebuild is invalid unless something GEOMETRIC is authored (a turn-type
  // override survives a rebuild, so it is not a reason to offer one).
  maneuvers_rebuild_button_->setEnabled(arm_based && anything_geometric);
  maneuvers_group_->show();
}

void PropertiesPanel::set_signal_tool(SignalTool* tool) {
  signal_tool_ = tool;
  if (signal_tool_ != nullptr) {
    connect(signal_tool_,
            &SignalTool::signalization_changed,
            this,
            &PropertiesPanel::refresh_signalization);
  }
  refresh_signalization();
}

void PropertiesPanel::refresh_signalization() {
  // deleteLater, not delete, for the reason refresh_maneuvers spells out: a row
  // editor pushes its command from inside its own signal, the command re-meshes,
  // and the re-mesh lands right back here.
  while (QLayoutItem* item = signalization_rows_layout_->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      row->setParent(nullptr);
      row->deleteLater();
    }
    delete item;
  }

  const RoadNetwork& network = document_.network();
  const JunctionId junction = selection_.primary().junction;
  // The section belongs to the Signal tool, so it appears only once one is
  // wired up — and only for a junction selection.
  if (signal_tool_ == nullptr || !junction.is_valid() || network.junction(junction) == nullptr) {
    signalization_group_->hide();
    return;
  }

  // The tool learns the new target from the SAME selection_changed signal this
  // panel does, and it connected later, so on the selection pass its target can
  // still be the previous junction. Its own signalization_changed then calls us
  // back with the target caught up — until then the commands stay disabled
  // rather than acting on the wrong junction.
  const bool tool_on_target = signal_tool_->inspected_junction() == junction;

  // Programmatic re-seed must not echo a set_pending_* back through the combo.
  {
    const QSignalBlocker block(signalization_template_combo_);
    signalization_template_combo_->setCurrentIndex(
        static_cast<int>(signal_tool_->pending_template()));
  }
  {
    const QSignalBlocker block(signalization_mount_combo_);
    const QString mount = QString::fromStdString(signal_tool_->pending_mount_model());
    const int index = signalization_mount_combo_->findData(mount);
    signalization_mount_combo_->setCurrentIndex(index >= 0 ? index : 0);
  }

  // Disabled, never failing: a span/virtual junction, a foreign one, and a
  // re-apply of exactly the applied template are all refusals the factory
  // would raise, so the button states them instead.
  const QString blocker = signal_tool_->signalize_blocker();
  signalization_apply_button_->setEnabled(tool_on_target && blocker.isEmpty());
  signalization_apply_button_->setToolTip(
      blocker.isEmpty() ? tr("Place this junction's signals and controllers from the template "
                             "above, as one undo step")
                        : blocker);
  signalization_clear_button_->setEnabled(tool_on_target && signal_tool_->can_clear());

  const std::optional<edit::SignalizeTemplate> applied = signal_tool_->applied_template();
  const auto template_name = [this](edit::SignalizeTemplate tmpl) {
    return signalization_template_combo_->itemText(static_cast<int>(tmpl));
  };
  auto* applied_row = new QLabel(
      applied.has_value() ? tr("Applied: %1").arg(template_name(*applied)) : tr("Applied: none"),
      signalization_group_);
  applied_row->setObjectName(QStringLiteral("signalization_applied_label"));
  signalization_rows_layout_->addWidget(applied_row);

  const auto road_name = [&network](RoadId road) {
    const Road* ptr = network.road(road);
    return ptr != nullptr ? QString::fromStdString(ptr->odr_id) : tr("?");
  };

  // One READ-ONLY row per approach, from the one query every consumer shares.
  // Solved against the junction under selection, not a value captured when the
  // rows were built.
  for (const JunctionApproachInfo& approach : junction_signals(network, junction)) {
    QStringList groups;
    for (const std::string& controller : approach.controller_odr_ids) {
      groups << QString::fromStdString(controller);
    }
    auto* row = new QLabel(
        tr("Arm %1 \u2014 %2 signal(s)%3, gates %4 turn(s)")
            .arg(road_name(approach.arm.road))
            .arg(approach.signal_ids.size())
            .arg(groups.isEmpty() ? tr(", no group") : tr(", group %1").arg(groups.join(", ")))
            .arg(approach.gated.size()),
        signalization_group_);
    row->setObjectName(QStringLiteral("signalization_approach_row"));
    row->setToolTip(approach.dynamic ? tr("Light-controlled approach")
                                     : tr("Sign-controlled (or unsignalized) approach"));
    signalization_rows_layout_->addWidget(row);
  }
  signalization_group_->show();
}

void PropertiesPanel::set_corner_tool(CornerTool* tool) {
  corner_tool_ = tool;
  if (corner_tool_ != nullptr) {
    connect(corner_tool_,
            &CornerTool::corner_selection_changed,
            this,
            &PropertiesPanel::refresh_corner);
  }
  refresh_corner();
}

std::optional<JunctionCornerInfo> PropertiesPanel::active_corner_info() const {
  if (corner_tool_ == nullptr) {
    return std::nullopt;
  }
  const auto corner = corner_tool_->active_corner();
  // The pane edits the corner of the junction it is SHOWING: a stale active
  // corner from a junction the user has since deselected must not be editable
  // through rows describing something else.
  if (!corner.has_value() || corner->junction != selection_.primary().junction) {
    return std::nullopt;
  }
  return corner_tool_->active_corner_info();
}

void PropertiesPanel::refresh_corner() {
  const std::optional<JunctionCornerInfo> info = active_corner_info();
  if (!info.has_value()) {
    corner_group_->hide();
    return;
  }
  corner_group_->show();

  corner_arms_label_->setText(tr("%1 ↔ %2")
                                  .arg(arm_name(document_.network(), info->arm_a))
                                  .arg(arm_name(document_.network(), info->arm_b)));

  // The overlay materials are seeded BEFORE the radius row's editability gate:
  // a corner too tight to carry a radius can still be painted, so the slots
  // must survive the early return below.
  corner_sidewalk_slot_->set_item(QString::fromStdString(info->sidewalk_material));
  corner_median_slot_->set_item(QString::fromStdString(info->median_material));

  // A corner whose arm faces leave no room below the floor cannot be edited
  // here — the row goes away rather than offering an empty range.
  const bool editable = info->max_radius >= kCornerRadiusMin;
  corner_form_->setRowVisible(corner_radius_spin_, editable);
  if (!editable) {
    return;
  }
  corner_radius_spin_->setRange(kCornerRadiusMin, info->max_radius);
  // On a geometrically tight corner the DERIVED radius already equals
  // max_radius, so the box opens at its own ceiling and typing a larger number
  // does nothing. That is the truth about the junction, not a bug: a value
  // above the bound is clamped when the floor is meshed. Say so.
  corner_radius_spin_->setToolTip(
      tr("Fillet radius. The arm faces leave room for at most %1 m at this corner; a larger "
         "value is clamped when the junction is meshed. Drag the attribute name to scrub.")
          .arg(info->max_radius, 0, 'f', 2));
  const QSignalBlocker blocker(corner_radius_spin_);
  corner_radius_spin_->setValue(info->radius);
}

std::optional<JunctionStopLineInfo> PropertiesPanel::active_stopline_info() const {
  if (stopline_tool_ == nullptr) {
    return std::nullopt;
  }
  const auto line = stopline_tool_->active_stopline();
  // The pane edits the stop line of the junction it is SHOWING: a stale active
  // line from a junction the user has since deselected must not be editable
  // through rows describing something else.
  if (!line.has_value() || line->junction != selection_.primary().junction) {
    return std::nullopt;
  }
  return stopline_tool_->active_stopline_info();
}

void PropertiesPanel::refresh_stopline() {
  const std::optional<JunctionStopLineInfo> info = active_stopline_info();
  if (!info.has_value()) {
    stopline_group_->hide();
    return;
  }
  stopline_group_->show();

  stopline_arm_label_->setText(
      tr("%1 · %2")
          .arg(arm_name(document_.network(), info->arm))
          .arg(info->flipped ? tr("outgoing lanes") : tr("approach lanes")));

  stopline_distance_spin_->setRange(0.0, info->max_distance);
  stopline_distance_spin_->setToolTip(
      tr("Setback from the junction mouth. This arm leaves room for at most %1 m; a larger "
         "value is clamped when the road is meshed. Drag the attribute name to scrub.")
          .arg(info->max_distance, 0, 'f', 2));
  {
    const QSignalBlocker blocker(stopline_distance_spin_);
    stopline_distance_spin_->setValue(info->distance);
  }

  // Nothing authored means nothing to reset — the arm is already showing the
  // derived default.
  stopline_reset_button_->setEnabled(info->authored);
}

void PropertiesPanel::refresh_junction(const Junction& junction) {
  // State is DERIVED from arms/spans/locked (road/junction.hpp), never stored.
  const bool span = !junction.spans.empty();
  const bool foreign = !span && junction.arms.empty();
  junction_type_label_->setText(span              ? tr("Span (virtual)")
                                : foreign         ? tr("Foreign")
                                : junction.locked ? tr("Locked")
                                                  : tr("Automatic"));
  {
    const QSignalBlocker blocker(junction_locked_check_);
    junction_locked_check_->setChecked(junction.locked || span);
  }
  // Only the two arm-based states can toggle: a span junction is locked
  // structurally and a foreign one has no derivation to guard.
  junction_locked_check_->setEnabled(!span && !foreign);
  {
    // Programmatic seeding must not echo a set/clear back through
    // editingFinished — the same guard every other editor here uses.
    const QSignalBlocker blocker(junction_radius_spin_);
    // No default → the minimum, which renders as "Derived" (specialValueText).
    junction_radius_spin_->setValue(junction.default_corner_radius.value_or(0.0));
  }
  junction_material_slot_->set_item(QString::fromStdString(junction.material));
  junction_group_->show();
}

void PropertiesPanel::refresh_lane_section() {
  const Lane* lane = primary_lane();
  const bool lane_selected = lane != nullptr;
  const bool center = lane_selected && lane->odr_id == 0;

  // Add-lane buttons act on the section, so a road-level selection suffices.
  add_left_->setEnabled(true);
  add_right_->setEnabled(true);

  type_combo_->setEnabled(lane_selected && !center);
  // The centre lane carries no material by rule (center_lane_no_material), so
  // the Materials slot only accepts a drop on a real lane.
  lane_material_slot_->setEnabled(lane_selected && !center);
  // The center lane is width-less by rule …road.lane.center_lane_no_width, and
  // a lane whose width varies along s is edited on the 2D Width curve — the
  // constant-width spin/scrub here would only be refused by set_lane_width.
  const bool width_constant =
      lane_selected && !center && !lane->widths.empty() && lane_width_is_constant(*lane);
  const bool width_tapered =
      lane_selected && !center && !lane->widths.empty() && !lane_width_is_constant(*lane);
  width_spin_->setEnabled(width_constant);
  if (width_scrub_label_ != nullptr) {
    width_scrub_label_->setEnabled(width_constant);
  }
  width_spin_->setToolTip(width_tapered
                              ? tr("Width varies along s — edit the curve in the 2D Editor (Width)")
                              : QString());
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
  // Lane 0's mark IS the center-line style — always editable on a lane, so the
  // marking slot (a write-only drop target for the variants beyond the combo's
  // Solid/Broken/None shortlist) is enabled on the centre lane too.
  mark_combo_->setEnabled(lane_selected);
  mark_width_spin_->setEnabled(lane_selected);
  marking_slot_->setEnabled(lane_selected);

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

bool PropertiesPanel::lane_width_is_constant(const Lane& lane) {
  // VERBATIM from edit::set_lane_width (core/src/edit/operations.cpp): a lane's
  // width is constant iff it has at most one record, and that record (if any)
  // has zero b/c/d. Kept identical so the UI's enable/disable and the kernel's
  // accept/refuse can never diverge.
  const std::vector<Poly3>& widths = lane.widths;
  return widths.size() <= 1 &&
         (widths.empty() ||
          (widths.front().b == 0.0 && widths.front().c == 0.0 && widths.front().d == 0.0));
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

namespace {
/// The asset editor's current widget values as a LibraryItem (for preview and
/// commit). `key`/material/category come from the tracked state; the geometry
/// from the spin boxes.
LibraryItem item_from_widgets(const QString& key,
                              const QString& material,
                              double width,
                              double border,
                              double dash,
                              double gap,
                              const QString& category) {
  LibraryItem item;
  item.key = key;
  item.kind = LibraryItem::Kind::Crosswalk;
  item.category = QStringLiteral("Crosswalks");
  item.crosswalk_width = width;
  item.crosswalk_border = border;
  item.crosswalk_dash = dash;
  item.crosswalk_gap = gap;
  item.crosswalk_material = material;
  item.crosswalk_segmentation = category;
  return item;
}
} // namespace

void PropertiesPanel::edit_asset(const QString& key, bool editable) {
  if (library_model_ == nullptr) {
    return;
  }
  const LibraryItem* item = library_model_->item_for_key(key);
  if (item != nullptr && item->kind == LibraryItem::Kind::PropSet) {
    edit_prop_set_asset(*item, editable);
    return;
  }
  if (item != nullptr && item->kind == LibraryItem::Kind::Tree) {
    edit_prop_asset(*item, editable);
    return;
  }
  if (item == nullptr || item->kind != LibraryItem::Kind::Crosswalk) {
    return; // a non-crosswalk / non-prop-set / non-prop key never opens the editor
  }
  asset_mode_ = true;
  asset_editable_ = editable;
  asset_key_ = key;
  asset_material_key_ = item->crosswalk_material;

  clear_rows();
  placeholder_->hide();
  name_row_->hide();
  lane_group_->hide();
  elevation_group_->hide();
  signal_group_->hide();
  object_group_->hide();
  style_group_->hide();
  surface_group_->hide();
  prop_set_group_->hide();
  prop_group_->hide();

  {
    const QSignalBlocker b1(asset_width_spin_);
    const QSignalBlocker b2(asset_border_spin_);
    const QSignalBlocker b3(asset_dash_spin_);
    const QSignalBlocker b4(asset_gap_spin_);
    const QSignalBlocker b5(asset_category_edit_);
    asset_width_spin_->setValue(item->crosswalk_width);
    asset_border_spin_->setValue(item->crosswalk_border);
    asset_dash_spin_->setValue(item->crosswalk_dash);
    asset_gap_spin_->setValue(item->crosswalk_gap);
    asset_category_edit_->setText(item->crosswalk_segmentation);
  }
  asset_material_slot_->set_item(item->crosswalk_material);

  for (QWidget* widget : {static_cast<QWidget*>(asset_width_spin_),
                          static_cast<QWidget*>(asset_border_spin_),
                          static_cast<QWidget*>(asset_dash_spin_),
                          static_cast<QWidget*>(asset_gap_spin_),
                          static_cast<QWidget*>(asset_category_edit_),
                          static_cast<QWidget*>(asset_material_slot_)}) {
    widget->setEnabled(editable);
  }
  asset_group_->setTitle(tr("Crosswalk asset — %1").arg(item->label));
  asset_hint_->setText(
      editable
          ? QString()
          : tr("Built-in asset (read-only). Use “New crosswalk asset…” to make an editable copy."));
  asset_hint_->setVisible(!editable);
  refresh_asset_preview();
  asset_group_->show();
}

void PropertiesPanel::refresh_asset_preview() {
  const LibraryItem item = item_from_widgets(asset_key_,
                                             asset_material_key_,
                                             asset_width_spin_->value(),
                                             asset_border_spin_->value(),
                                             asset_dash_spin_->value(),
                                             asset_gap_spin_->value(),
                                             asset_category_edit_->text());
  asset_preview_->setPixmap(render_crosswalk_preview(item, QSize(96, 48), materials_));
}

void PropertiesPanel::commit_asset_edit() {
  if (!asset_mode_ || !asset_editable_) {
    return;
  }
  refresh_asset_preview();
  emit crosswalk_asset_committed(item_from_widgets(asset_key_,
                                                   asset_material_key_,
                                                   asset_width_spin_->value(),
                                                   asset_border_spin_->value(),
                                                   asset_dash_spin_->value(),
                                                   asset_gap_spin_->value(),
                                                   asset_category_edit_->text()));
}

void PropertiesPanel::clear_prop_set_rows() {
  for (const PropSetRow& row : prop_set_rows_) {
    row.container->deleteLater();
  }
  prop_set_rows_.clear();
}

void PropertiesPanel::add_prop_set_row(const QString& model, double portion, bool editable) {
  auto* container = new QWidget(prop_set_group_);
  auto* row_layout = new QHBoxLayout(container);
  row_layout->setContentsMargins(0, 0, 0, 0);

  auto* model_combo = new QComboBox(container);
  model_combo->setObjectName(QStringLiteral("prop_set_model"));
  for (const std::string& id : props::ids()) {
    model_combo->addItem(QString::fromStdString(id));
  }
  const int index = model_combo->findText(model);
  if (index >= 0) {
    model_combo->setCurrentIndex(index);
  }

  auto* portion_spin = new QDoubleSpinBox(container);
  portion_spin->setObjectName(QStringLiteral("prop_set_portion"));
  portion_spin->setRange(0.1, 100.0);
  portion_spin->setSingleStep(0.5);
  portion_spin->setDecimals(1);
  portion_spin->setValue(portion);

  auto* remove_button = new QPushButton(tr("Remove"), container);
  remove_button->setObjectName(QStringLiteral("prop_set_remove"));

  row_layout->addWidget(model_combo, 1);
  row_layout->addWidget(portion_spin);
  row_layout->addWidget(remove_button);

  model_combo->setEnabled(editable);
  portion_spin->setEnabled(editable);
  remove_button->setEnabled(editable);

  prop_set_entries_layout_->addWidget(container);
  prop_set_rows_.push_back(
      PropSetRow{.container = container, .model = model_combo, .portion = portion_spin});

  connect(remove_button, &QPushButton::clicked, this, [this, container] {
    const auto it =
        std::find_if(prop_set_rows_.begin(),
                     prop_set_rows_.end(),
                     [container](const PropSetRow& row) { return row.container == container; });
    if (it != prop_set_rows_.end()) {
      it->container->deleteLater();
      prop_set_rows_.erase(it);
    }
  });
}

void PropertiesPanel::edit_prop_set_asset(const LibraryItem& item, bool editable) {
  // A subtype of asset mode: refresh() early-returns on asset_mode_ so a
  // background mesh_changed never tears the editor down mid-edit.
  asset_mode_ = true;
  prop_set_mode_ = true;
  asset_key_ = item.key;
  prop_set_label_ = item.label;
  prop_set_category_ = item.category;

  clear_rows();
  clear_prop_set_rows();
  placeholder_->hide();
  name_row_->hide();
  lane_group_->hide();
  elevation_group_->hide();
  signal_group_->hide();
  object_group_->hide();
  style_group_->hide();
  surface_group_->hide();
  asset_group_->hide();
  prop_group_->hide();

  for (const LibraryItem::PropSetEntry& entry : item.prop_entries) {
    add_prop_set_row(entry.model, entry.portion, editable);
  }
  prop_set_add_button_->setEnabled(editable);
  prop_set_save_button_->setEnabled(editable);
  prop_set_group_->setTitle(tr("Prop set — %1").arg(item.label));
  prop_set_hint_->setText(
      editable ? tr("Weighted scatter set: props are drawn per instance in proportion to "
                    "each entry's weight.")
               : tr("Built-in asset (read-only). Use “New prop set…” to make an editable copy."));
  prop_set_group_->show();
}

void PropertiesPanel::commit_prop_set_edit() {
  if (!prop_set_mode_) {
    return;
  }
  LibraryItem item;
  item.key = asset_key_;
  item.label = prop_set_label_;
  item.category = prop_set_category_;
  item.kind = LibraryItem::Kind::PropSet;
  for (const PropSetRow& row : prop_set_rows_) {
    item.prop_entries.push_back(LibraryItem::PropSetEntry{.model = row.model->currentText(),
                                                          .portion = row.portion->value()});
  }
  emit prop_set_asset_committed(item);
}

void PropertiesPanel::edit_prop_asset(const LibraryItem& item, bool editable) {
  // A subtype of asset mode (refresh() early-returns on asset_mode_). Unlike the
  // prop set, a single spin commits per-field on editingFinished.
  asset_mode_ = true;
  prop_mode_ = true;
  asset_editable_ = editable;
  asset_key_ = item.key;
  prop_asset_item_ = item; // the source copy the commit patches

  clear_rows();
  placeholder_->hide();
  name_row_->hide();
  lane_group_->hide();
  elevation_group_->hide();
  signal_group_->hide();
  object_group_->hide();
  style_group_->hide();
  surface_group_->hide();
  asset_group_->hide();
  prop_set_group_->hide();

  prop_group_->setTitle(tr("Prop asset — %1").arg(item.label));
  prop_model_label_->setText(item.model);
  {
    const QSignalBlocker block(prop_scale_spin_);
    prop_scale_spin_->setValue(item.default_scale);
  }
  prop_scale_spin_->setEnabled(editable);
  prop_scale_spin_->setReadOnly(!editable);
  refresh_prop_scale_hint();
  prop_hint_->setText(editable
                          ? tr("Uniform spawn multiplier applied to new placements of this prop.")
                          : tr("Open a project to edit prop defaults."));
  prop_group_->show();
}

void PropertiesPanel::commit_prop_asset_edit() {
  if (!prop_mode_ || !asset_editable_) {
    return;
  }
  LibraryItem item = prop_asset_item_;
  item.default_scale = prop_scale_spin_->value();
  // A parsed item re-emits its verbatim create_raw, so the edited scale is lost
  // unless we patch the raw block too (to_json reads create_raw when non-empty).
  if (!item.create_raw.isEmpty()) {
    item.create_raw[QStringLiteral("default_scale")] = item.default_scale;
  }
  prop_asset_item_ = item; // keep the source in sync for the next edit
  refresh_prop_scale_hint();
  emit prop_asset_committed(item);
}

void PropertiesPanel::refresh_prop_scale_hint() {
  const double scale = prop_scale_spin_->value();
  if (const props::PropModel* model = props::model(prop_asset_item_.model.toStdString())) {
    prop_scale_hint_->setText(tr("Spawns at %1 m (model %2 m)")
                                  .arg(model->height * scale, 0, 'f', 2)
                                  .arg(model->height, 0, 'f', 2));
  } else {
    prop_scale_hint_->clear();
  }
}

} // namespace roadmaker::editor
