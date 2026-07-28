/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "panels/world_georeference_window.hpp"

#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/units.hpp"
#include "viewport/framing.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/georeference.hpp"

namespace roadmaker::editor {

namespace {

/// Angle spins are degrees with six decimals — about 0.1 m of latitude, which
/// is finer than anything a road scene is placed to and coarse enough that the
/// box does not read as false precision. The stored value keeps full double
/// precision regardless; this is display only.
constexpr int kAngleDecimals = 6;

/// A metre spin covering the whole plausible range of a dataset offset,
/// including full UTM northings.
QDoubleSpinBox* make_metre_spin(QWidget* parent) {
  auto* spin = new QDoubleSpinBox(parent);
  spin->setRange(-100'000'000.0, 100'000'000.0);
  spin->setDecimals(3);
  spin->setSingleStep(1.0);
  return spin;
}

} // namespace

WorldGeoreferenceWindow::WorldGeoreferenceWindow(Document& document,
                                                 SelectionModel& selection,
                                                 QWidget* parent)
    : QWidget(parent, Qt::Window), document_(document), selection_(selection) {
  setWindowTitle(tr("World Georeference"));
  setAttribute(Qt::WA_DeleteOnClose);
  resize(560, 520);
  build_ui();
  refresh();

  connect(&document_, &Document::loaded, this, &WorldGeoreferenceWindow::refresh);
  connect(&document_, &Document::topology_changed, this, [this] { render_workspace(); });
  connect(&document_, &Document::mesh_changed, this, [this](const std::vector<RoadId>&) {
    render_workspace();
  });
  // The extent read-out is formatted text, so a unit-system flip has to
  // re-render it rather than wait for the next natural refresh.
  connect(&units::Notifier::instance(), &units::Notifier::changed, this, [this] {
    render_workspace();
  });
}

void WorldGeoreferenceWindow::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // ------------------------------------------------------------ projection
  auto* crs_group = new QGroupBox(tr("Projection"), this);
  auto* crs_layout = new QVBoxLayout(crs_group);

  origin_mode_ = new QRadioButton(tr("World origin (latitude / longitude)"), crs_group);
  custom_mode_ = new QRadioButton(tr("Custom CRS (proj-string or WKT)"), crs_group);
  origin_mode_->setChecked(true);
  crs_layout->addWidget(origin_mode_);

  auto* origin_form = new QFormLayout();
  latitude_ = new QDoubleSpinBox(crs_group);
  latitude_->setRange(-90.0, 90.0);
  latitude_->setDecimals(kAngleDecimals);
  latitude_->setSuffix(tr(" °"));
  longitude_ = new QDoubleSpinBox(crs_group);
  longitude_->setRange(-180.0, 180.0);
  longitude_->setDecimals(kAngleDecimals);
  longitude_->setSuffix(tr(" °"));
  origin_form->addRow(tr("Latitude"), latitude_);
  origin_form->addRow(tr("Longitude"), longitude_);
  crs_layout->addLayout(origin_form);

  auto* origin_note = new QLabel(
      tr("A Transverse Mercator centred on the scene's own origin, which OpenDRIVE §8.5 "
         "recommends. The scene's local coordinates are the projected coordinates, so nothing "
         "is transformed."),
      crs_group);
  origin_note->setWordWrap(true);
  crs_layout->addWidget(origin_note);

  crs_layout->addWidget(custom_mode_);
  projection_text_ = new QPlainTextEdit(crs_group);
  projection_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  projection_text_->setPlaceholderText(tr("+proj=utm +zone=32 +datum=WGS84 +units=m +no_defs"));
  projection_text_->setMaximumHeight(70);
  crs_layout->addWidget(projection_text_);

  auto* custom_note =
      new QLabel(tr("Stored and exported verbatim. This build carries a projection it did not "
                    "author without interpreting it, so the world origin is not resolved for a "
                    "custom CRS."),
                 crs_group);
  custom_note->setWordWrap(true);
  crs_layout->addWidget(custom_note);
  layout->addWidget(crs_group);

  // ---------------------------------------------------------------- offset
  auto* offset_group = new QGroupBox(tr("Dataset offset (§8.5)"), this);
  auto* offset_layout = new QVBoxLayout(offset_group);
  auto* offset_form = new QFormLayout();
  offset_x_ = make_metre_spin(offset_group);
  offset_y_ = make_metre_spin(offset_group);
  offset_z_ = make_metre_spin(offset_group);
  offset_hdg_ = new QDoubleSpinBox(offset_group);
  offset_hdg_->setRange(-6.2831853071795862, 6.2831853071795862);
  offset_hdg_->setDecimals(6);
  offset_form->addRow(tr("x [m]"), offset_x_);
  offset_form->addRow(tr("y [m]"), offset_y_);
  offset_form->addRow(tr("z [m]"), offset_z_);
  offset_form->addRow(tr("Heading [rad]"), offset_hdg_);
  offset_layout->addLayout(offset_form);
  clear_offset_ = new QPushButton(tr("Clear offset"), offset_group);
  offset_layout->addWidget(clear_offset_);
  layout->addWidget(offset_group);

  // ------------------------------------------------------------- workspace
  auto* workspace_group = new QGroupBox(tr("Workspace"), this);
  auto* workspace_layout = new QVBoxLayout(workspace_group);
  workspace_summary_ = new QLabel(workspace_group);
  workspace_summary_->setWordWrap(true);
  workspace_layout->addWidget(workspace_summary_);
  auto* fit_button = new QPushButton(tr("Fit workspace to selection"), workspace_group);
  workspace_layout->addWidget(fit_button);
  layout->addWidget(workspace_group);

  summary_ = new QLabel(this);
  summary_->setWordWrap(true);
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(summary_);

  auto* buttons = new QHBoxLayout();
  buttons->addStretch();
  auto* clear_button = new QPushButton(tr("Clear georeference"), this);
  auto* apply_button = new QPushButton(tr("Apply"), this);
  apply_button->setDefault(true);
  buttons->addWidget(clear_button);
  buttons->addWidget(apply_button);
  layout->addLayout(buttons);

  connect(origin_mode_, &QRadioButton::toggled, this, [this] { sync_mode(); });
  connect(apply_button, &QPushButton::clicked, this, [this] { apply(); });
  connect(fit_button, &QPushButton::clicked, this, [this] { fit_workspace_to_selection(); });
  connect(clear_offset_, &QPushButton::clicked, this, [this] {
    offset_x_->setValue(0.0);
    offset_y_->setValue(0.0);
    offset_z_->setValue(0.0);
    offset_hdg_->setValue(0.0);
  });
  connect(clear_button, &QPushButton::clicked, this, [this] {
    // Clearing is setting the empty value, which is what the command layer
    // models — there is no separate remove. A refusal here means the scene had
    // no georeference to clear, which is not worth telling anyone about.
    (void)document_.push_command(edit::set_georeference(document_.network(), GeoReference{}));
    refresh();
  });
  sync_mode();
}

void WorldGeoreferenceWindow::sync_mode() {
  const bool by_origin = origin_mode_->isChecked();
  latitude_->setEnabled(by_origin);
  longitude_->setEnabled(by_origin);
  projection_text_->setEnabled(!by_origin);
}

std::string WorldGeoreferenceWindow::form_projection() const {
  if (custom_mode_->isChecked()) {
    return projection_text_->toPlainText().trimmed().toStdString();
  }
  const auto proj = tmerc_projection(latitude_->value(), longitude_->value());
  // The spins are range-limited to the globe, so this cannot fail; returning
  // the empty string rather than asserting keeps a future range change from
  // becoming a crash.
  return proj.has_value() ? *proj : std::string{};
}

void WorldGeoreferenceWindow::refresh() {
  loading_ = true;
  const GeoReference& geo = document_.network().georeference();

  if (const auto origin = tmerc_origin(geo.projection)) {
    // A projection this build authored: show it as an origin, which is the
    // form the user gave it in.
    origin_mode_->setChecked(true);
    latitude_->setValue((*origin)[0]);
    longitude_->setValue((*origin)[1]);
    projection_text_->setPlainText(QString::fromStdString(geo.projection));
  } else if (!geo.projection.empty()) {
    custom_mode_->setChecked(true);
    projection_text_->setPlainText(QString::fromStdString(geo.projection));
  } else {
    origin_mode_->setChecked(true);
    projection_text_->clear();
  }

  const GeoOffset offset = geo.offset.value_or(GeoOffset{});
  offset_x_->setValue(offset.x);
  offset_y_->setValue(offset.y);
  offset_z_->setValue(offset.z);
  offset_hdg_->setValue(offset.hdg);

  if (geo.empty()) {
    summary_->setText(tr("This scene has no georeference — its coordinates are a local "
                         "Cartesian frame, which is what a reader assumes when the "
                         "definition is missing."));
  } else if (tmerc_origin(geo.projection).has_value()) {
    summary_->setText(tr("Georeferenced. The .xodr carries this in <header><geoReference>."));
  } else {
    summary_->setText(tr("Georeferenced with a projection this build carries but does not "
                         "read. It exports verbatim."));
  }

  sync_mode();
  render_workspace();
  loading_ = false;
}

void WorldGeoreferenceWindow::render_workspace() {
  const std::optional<SceneWorkspaceState>& workspace = document_.scene_state().workspace;
  if (!workspace.has_value()) {
    workspace_summary_->setText(
        tr("No workspace framed. Fitting one records the working area beside the scene."));
    return;
  }
  const std::array<double, 4>& box = workspace->extents;
  workspace_summary_->setText(tr("Workspace: %1 east-west by %2 north-south, centred on "
                                 "(%3, %4).")
                                  .arg(units::format_length(box[2] - box[0]),
                                       units::format_length(box[3] - box[1]),
                                       units::format_length((box[0] + box[2]) / 2.0),
                                       units::format_length((box[1] + box[3]) / 2.0)));
}

bool WorldGeoreferenceWindow::apply() {
  GeoReference geo;
  geo.projection = form_projection();
  const GeoOffset offset{.x = offset_x_->value(),
                         .y = offset_y_->value(),
                         .z = offset_z_->value(),
                         .hdg = offset_hdg_->value()};
  // An identity offset is stored as absence, so the form's "all zeros" and
  // "no offset" agree — otherwise clearing the spins would leave a value the
  // writer then declines to emit, and the two would disagree forever.
  if (!offset.identity()) {
    geo.offset = offset;
  }

  const bool applied =
      document_.push_command(edit::set_georeference(document_.network(), geo)).has_value();
  refresh();
  return applied;
}

void WorldGeoreferenceWindow::fit_workspace_to_selection() {
  // Layer 2, so this is NOT a command: the workspace is framing, and losing it
  // costs a view rather than a road. Putting it on the undo stack would also
  // interleave view changes with edits in the one history the user relies on.
  SceneBounds bounds = selection_.empty()
                           ? SceneBounds{}
                           : selection_bounds(document_.mesh(), selection_.entries());
  if (!bounds.valid()) {
    // Nothing selected, or a selection the mesh cannot resolve: frame the
    // whole network, which is the same fallback frame_selection() uses.
    if (const auto plan = network_plan_bounds(document_.network())) {
      SceneState state = document_.scene_state();
      state.workspace = SceneWorkspaceState{
          .extents = *plan, .crs = document_.network().georeference().projection};
      document_.set_scene_state(std::move(state));
      render_workspace();
    }
    return;
  }

  SceneState state = document_.scene_state();
  state.workspace = SceneWorkspaceState{
      .extents = {static_cast<double>(bounds.lo[0]),
                  static_cast<double>(bounds.lo[1]),
                  static_cast<double>(bounds.hi[0]),
                  static_cast<double>(bounds.hi[1])},
      // The frame the box is being recorded in. Without it a later
      // re-georeferencing would leave these numbers describing somewhere else,
      // silently.
      .crs = document_.network().georeference().projection};
  document_.set_scene_state(std::move(state));
  render_workspace();
}

} // namespace roadmaker::editor
