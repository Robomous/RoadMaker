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

#include "panels/export_preview_window.hpp"

#include "roadmaker/road/georeference.hpp"

#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include "document/document.hpp"
#include "document/units.hpp"

namespace roadmaker::editor {
namespace {

QTableView* make_table(QAbstractItemModel* model, QWidget* parent) {
  auto* view = new QTableView(parent);
  view->setModel(model);
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view->verticalHeader()->setVisible(false);
  view->horizontalHeader()->setStretchLastSection(true);
  view->setAlternatingRowColors(true);
  return view;
}

} // namespace

ExportPreviewWindow::ExportPreviewWindow(Document& document, QWidget* parent)
    : QWidget(parent, Qt::Window), document_(document) {
  setWindowTitle(tr("Export Preview"));
  setAttribute(Qt::WA_DeleteOnClose);
  resize(880, 620);
  build_ui();

  connect(&document_, &Document::mesh_changed, this, [this](const std::vector<RoadId>&) {
    mark_stale();
  });
  connect(&document_, &Document::topology_changed, this, &ExportPreviewWindow::mark_stale);
  // A prop placement moves no road, so it arrives on its own channel. Without
  // this the manifest silently under-reports props.
  connect(&document_, &Document::objects_changed, this, [this](const std::vector<RoadId>&) {
    mark_stale();
  });
  connect(&document_, &Document::loaded, this, &ExportPreviewWindow::mark_stale);

  // Lengths are cached as formatted text, so they must be re-rendered when the
  // unit system flips rather than waiting for the next natural refresh.
  connect(&units::Notifier::instance(), &units::Notifier::changed, this, [this] {
    if (state_.computed) {
      render();
    }
  });
}

void ExportPreviewWindow::build_ui() {
  tabs_ = new QTabWidget(this);

  // ---------------------------------------------------------- scene page
  auto* scene_page = new QWidget(tabs_);
  auto* scene_layout = new QVBoxLayout(scene_page);

  auto* format_row = new QHBoxLayout();
  format_row->addWidget(new QLabel(tr("Format:"), scene_page));
  auto* format_box = new QComboBox(scene_page);
  format_box->addItem(tr("glTF (.glb)"), QVariant::fromValue(0));
  format_box->addItem(tr("OpenUSD (.usda)"), QVariant::fromValue(1));
  format_row->addWidget(format_box);
  format_row->addStretch();
  auto* refresh_button = new QPushButton(tr("Refresh"), scene_page);
  format_row->addWidget(refresh_button);
  scene_layout->addLayout(format_row);

  availability_note_ = new QLabel(scene_page);
  availability_note_->setWordWrap(true);
  availability_note_->setVisible(false);
  scene_layout->addWidget(availability_note_);

  stale_note_ = new QLabel(tr("The scene has changed since this preview was taken."), scene_page);
  stale_note_->setWordWrap(true);
  stale_note_->setVisible(false);
  scene_layout->addWidget(stale_note_);

  channel_view_ = make_table(&channel_model_, scene_page);
  scene_layout->addWidget(channel_view_, /*stretch=*/2);

  scene_summary_ = new QLabel(scene_page);
  scene_summary_->setWordWrap(true);
  scene_summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  scene_layout->addWidget(scene_summary_);

  scene_layout->addWidget(new QLabel(tr("Materials"), scene_page));
  material_view_ = make_table(&material_model_, scene_page);
  scene_layout->addWidget(material_view_, /*stretch=*/1);

  tabs_->addTab(scene_page, tr("Scene"));

  // ------------------------------------------------------ opendrive page
  auto* xodr_page = new QWidget(tabs_);
  auto* xodr_layout = new QVBoxLayout(xodr_page);

  xodr_summary_ = new QLabel(xodr_page);
  xodr_summary_->setWordWrap(true);
  xodr_summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  xodr_layout->addWidget(xodr_summary_);

  xodr_layout->addWidget(new QLabel(tr("RoadMaker extensions carried in the file"), xodr_page));
  record_view_ = make_table(&record_model_, xodr_page);
  xodr_layout->addWidget(record_view_, /*stretch=*/1);

  xodr_layout->addWidget(new QLabel(tr("OpenDRIVE as it will be written"), xodr_page));
  xml_view_ = new QPlainTextEdit(xodr_page);
  xml_view_->setReadOnly(true);
  xml_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
  xml_view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  xodr_layout->addWidget(xml_view_, /*stretch=*/3);

  tabs_->addTab(xodr_page, tr("OpenDRIVE"));

  auto* outer = new QVBoxLayout(this);
  outer->addWidget(tabs_);

  connect(refresh_button, &QPushButton::clicked, this, &ExportPreviewWindow::refresh);
  connect(format_box, &QComboBox::currentIndexChanged, this, [this](int index) {
    set_scene_format(index == 1 ? MeshExportFormat::Usd : MeshExportFormat::Gltf);
  });
}

void ExportPreviewWindow::set_scene_format(MeshExportFormat format) {
  scene_format_ = format;
  if (state_.computed) {
    render_scene();
  }
}

void ExportPreviewWindow::mark_stale() {
  state_.stale = true;
  if (stale_note_ != nullptr) {
    stale_note_->setVisible(state_.computed);
  }
}

void ExportPreviewWindow::refresh() {
  // The validator's view of the CURRENT network, republished so the
  // Diagnostics dock agrees with what this window is about to say. Before
  // #241 this ran only inside save().
  document_.refresh_diagnostics();
  recompute_export_preview(document_, state_);
  render();
}

void ExportPreviewWindow::show_page(Page page) {
  if (!state_.computed || state_.stale) {
    refresh();
  }
  tabs_->setCurrentIndex(page == Page::OpenDrive ? 1 : 0);
  show();
  raise();
  activateWindow();
}

void ExportPreviewWindow::render() {
  render_scene();
  render_xodr();
  stale_note_->setVisible(state_.stale && state_.computed);
}

void ExportPreviewWindow::render_scene() {
  const ScenePreview& preview = scene_format_ == MeshExportFormat::Usd ? state_.usd : state_.gltf;
  channel_model_.set_preview(&preview);
  material_model_.set_preview(&preview);
  channel_view_->resizeColumnsToContents();

  availability_note_->setVisible(!preview.available);
  if (!preview.available) {
    // Strictly more informative than hiding the page: the manifest is still
    // correct, it is only this build that cannot write the file.
    availability_note_->setText(
        tr("This build cannot write .usda (RM_BUILD_USD is off). The manifest below is what a "
           "USD-enabled build would produce."));
  }

  QString summary;
  if (!preview.would_export) {
    summary =
        tr("Nothing would be exported: %1")
            .arg(preview.refusal.has_value() ? QString::fromStdString(preview.refusal->message)
                                             : tr("the mesh is empty"));
  } else {
    summary = tr("%1 triangles over %2 vertices, in %3 meshes and %4 nodes.")
                  .arg(format_count(preview.total_triangles),
                       format_count(preview.total_vertices),
                       format_count(preview.mesh_count),
                       format_count(preview.node_count));
    if (preview.image_count > 0) {
      summary += tr(" %1 embedded image(s).").arg(format_count(preview.image_count));
    }
    if (preview.bounds.valid) {
      // Export frame, said out loud: these axes are Y-up, not the viewport's
      // Z-up kernel frame, so a user comparing them is not misled.
      summary += tr("\nExtents (export frame, Y up): %1 × %2 × %3")
                     .arg(units::format_length(preview.bounds.max[0] - preview.bounds.min[0]),
                          units::format_length(preview.bounds.max[1] - preview.bounds.min[1]),
                          units::format_length(preview.bounds.max[2] - preview.bounds.min[2]));
    }
  }
  for (const Diagnostic& note : preview.notes) {
    summary += "\n⚠ " + QString::fromStdString(note.message);
  }
  scene_summary_->setText(summary);
}

void ExportPreviewWindow::render_xodr() {
  const XodrPreview& preview = state_.xodr;
  record_model_.set_preview(&preview);
  record_view_->resizeColumnsToContents();

  if (!preview.would_write) {
    QString text =
        tr("The file would not be written: %1")
            .arg(preview.refusal.has_value() ? QString::fromStdString(preview.refusal->message)
                                             : tr("unknown error"));
    // The whole point of running the advisory sweep first: the refusal is one
    // message, the findings are all of them.
    text += tr("\n%1 checker finding(s) — see the Diagnostics panel.")
                .arg(format_count(preview.diagnostics.size()));
    xodr_summary_->setText(text);
    xml_view_->setPlainText({});
    return;
  }

  QString summary = tr("%1 roads (%2 of reference line), %3 junctions, %4 lane sections, "
                       "%5 lanes, %6 objects, %7 signals — %8 bytes.")
                        .arg(format_count(preview.road_count),
                             units::format_length(preview.total_reference_length),
                             format_count(preview.junction_count),
                             format_count(preview.lane_section_count),
                             format_count(preview.lane_count),
                             format_count(preview.object_count),
                             format_count(preview.signal_count),
                             format_count(preview.byte_count));
  summary += tr("\n%1 checker finding(s) — see the Diagnostics panel.")
                 .arg(format_count(preview.diagnostics.size()));
  // The <header> read-out (p7-s5, #324). Georeferencing is the difference
  // between a scene that sits somewhere and one that sits nowhere, so a preview
  // that stays silent about it would let a scene ship unplaced without saying
  // so — the same class of omission #390 records for the ground channels.
  if (preview.header.geo_reference.empty()) {
    summary += tr("\nNo georeference — the file describes a local Cartesian frame.");
  } else if (const auto origin = tmerc_origin(preview.header.geo_reference)) {
    summary += tr("\nWorld origin: %1, %2 (Transverse Mercator).")
                   .arg((*origin)[0], 0, 'f', 6)
                   .arg((*origin)[1], 0, 'f', 6);
  } else {
    // A CRS this build carries but does not interpret. Saying so is the honest
    // report; naming an origin we cannot compute would be a guess.
    summary += tr("\nProjection (carried verbatim): %1")
                   .arg(QString::fromStdString(preview.header.geo_reference));
  }
  if (preview.header.offset.has_value()) {
    summary += tr("\nDataset offset: x %1, y %2, z %3, heading %4 rad.")
                   .arg(units::format_length(preview.header.offset->x),
                        units::format_length(preview.header.offset->y),
                        units::format_length(preview.header.offset->z))
                   .arg(preview.header.offset->hdg, 0, 'f', 4);
  }
  if (preview.header.bounds.has_value()) {
    const std::array<double, 4>& box = *preview.header.bounds;
    summary +=
        tr("\nExtent: %1 east-west by %2 north-south.")
            .arg(units::format_length(box[2] - box[0]), units::format_length(box[3] - box[1]));
  }
  if (!preview.terrain_sidecar.empty()) {
    summary += tr("\nA terrain sidecar would be written beside it: %1")
                   .arg(QString::fromStdString(preview.terrain_sidecar));
  }
  if (!preview.foreign_user_data_codes.empty()) {
    QStringList codes;
    for (const std::string& code : preview.foreign_user_data_codes) {
      codes << QString::fromStdString(code);
    }
    summary += tr("\nForeign userData preserved verbatim: %1").arg(codes.join(", "));
  }
  xodr_summary_->setText(summary);
  xml_view_->setPlainText(QString::fromStdString(preview.xml));
}

} // namespace roadmaker::editor
