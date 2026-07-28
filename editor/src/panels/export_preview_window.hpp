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

#pragma once

// The Scene Export Preview and OpenDRIVE Export Preview tools (p7-s1, #241) —
// GW-2 steps 21 and 22.
//
// A non-modal top-level window rather than a dock or a dialog. The editor has
// no QDialog subclasses and this is not the place to add the first; the only
// precedent for a tool window is help::HelpViewer, which MainWindow holds by
// QPointer and builds lazily. A dock would additionally cost a permanent slot
// in the saved layout, a help-registry row, and an entry in the canonical
// dock-name list — for a window opened twice per project.

#include <QPointer>
#include <QWidget>

#include "document/export_preview_models.hpp"
#include "document/export_preview_state.hpp"

class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableView;

namespace roadmaker::editor {

class Document;

class ExportPreviewWindow : public QWidget {
  Q_OBJECT

public:
  /// Which tool the window was opened as. Both live in one window because they
  /// answer the same question about the same scene, but each menu entry opens
  /// its own page so GW-2's two steps have two distinct entry points.
  enum class Page {
    Scene,
    OpenDrive,
  };

  explicit ExportPreviewWindow(Document& document, QWidget* parent = nullptr);

  /// Brings the window up on `page`, recomputing if it has never been computed
  /// or has gone stale.
  void show_page(Page page);

  /// Recomputes both previews from the document. Public so tests are
  /// deterministic rather than waiting on a paint.
  void refresh();

  /// Marks the preview out of date without recomputing. Wired to the
  /// document's change signals — including objects_changed, which is a
  /// SEPARATE DirtySet channel: a prop placement moves no road, so a window
  /// listening only to mesh_changed would silently under-report props.
  void mark_stale();

  // --- test seams -------------------------------------------------------
  [[nodiscard]] QTabWidget* tabs() { return tabs_; }

  [[nodiscard]] QTableView* channel_view() { return channel_view_; }

  [[nodiscard]] QTableView* material_view() { return material_view_; }

  [[nodiscard]] QTableView* record_view() { return record_view_; }

  [[nodiscard]] QPlainTextEdit* xml_view() { return xml_view_; }

  [[nodiscard]] QLabel* scene_summary() { return scene_summary_; }

  [[nodiscard]] QLabel* xodr_summary() { return xodr_summary_; }

  [[nodiscard]] QLabel* availability_note() { return availability_note_; }

  [[nodiscard]] QLabel* stale_note() { return stale_note_; }

  [[nodiscard]] const ExportPreviewState& state() const { return state_; }

  [[nodiscard]] ExportChannelModel& channel_model() { return channel_model_; }

  [[nodiscard]] ExportMaterialModel& material_model() { return material_model_; }

  [[nodiscard]] XodrRecordModel& record_model() { return record_model_; }

  /// The mesh format the Scene page is currently describing.
  [[nodiscard]] MeshExportFormat scene_format() const { return scene_format_; }

  void set_scene_format(MeshExportFormat format);

private:
  void build_ui();
  void render();
  void render_scene();
  void render_xodr();

  Document& document_;
  ExportPreviewState state_;
  MeshExportFormat scene_format_ = MeshExportFormat::Gltf;

  QTabWidget* tabs_ = nullptr;
  QTableView* channel_view_ = nullptr;
  QTableView* material_view_ = nullptr;
  QTableView* record_view_ = nullptr;
  QPlainTextEdit* xml_view_ = nullptr;
  QLabel* scene_summary_ = nullptr;
  QLabel* xodr_summary_ = nullptr;
  QLabel* availability_note_ = nullptr;
  QLabel* stale_note_ = nullptr;

  ExportChannelModel channel_model_;
  ExportMaterialModel material_model_;
  XodrRecordModel record_model_;
};

} // namespace roadmaker::editor
