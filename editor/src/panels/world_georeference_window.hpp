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

// The World Georeference tool (p7-s5, #324) — where the scene sits on the
// earth, and the working area it is framed against.
//
// A non-modal top-level window, following ExportPreviewWindow for the same
// stated reasons: the editor has no QDialog subclasses and this is not the
// place to add the first, and a dock would cost a permanent saved-layout slot
// for a window opened once per project.
//
// The window edits two things that live in DIFFERENT persistence layers, which
// is why they share a window rather than a data structure:
//   - the georeference (projection + offset) is Layer 0 — it travels in the
//     .xodr's <header> and is committed through the undo stack like any edit;
//   - the workspace box is Layer 2 — it is framing, lives in the scene
//     sidecar, and is not undoable because losing it costs a view, not a road.

#include <QWidget>

#include <array>
#include <optional>

class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;

namespace roadmaker::editor {

class Document;
class SelectionModel;

class WorldGeoreferenceWindow : public QWidget {
  Q_OBJECT

public:
  WorldGeoreferenceWindow(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  /// Re-reads the document into the controls. Public so tests are
  /// deterministic rather than waiting on a paint.
  void refresh();

  /// Commits the georeference as ONE undoable command. Returns false when the
  /// form describes what the document already holds, which the command layer
  /// refuses as a no-op.
  bool apply();

  /// Frames the workspace on the current selection, or on the whole network
  /// when nothing is selected. Layer 2 — no command, no undo entry.
  void fit_workspace_to_selection();

  // --- test seams -------------------------------------------------------
  [[nodiscard]] QRadioButton* origin_mode() { return origin_mode_; }

  [[nodiscard]] QRadioButton* custom_mode() { return custom_mode_; }

  [[nodiscard]] QDoubleSpinBox* latitude() { return latitude_; }

  [[nodiscard]] QDoubleSpinBox* longitude() { return longitude_; }

  [[nodiscard]] QPlainTextEdit* projection_text() { return projection_text_; }

  [[nodiscard]] QDoubleSpinBox* offset_x() { return offset_x_; }

  [[nodiscard]] QDoubleSpinBox* offset_y() { return offset_y_; }

  [[nodiscard]] QDoubleSpinBox* offset_z() { return offset_z_; }

  [[nodiscard]] QDoubleSpinBox* offset_hdg() { return offset_hdg_; }

  [[nodiscard]] QLabel* summary() { return summary_; }

  [[nodiscard]] QLabel* workspace_summary() { return workspace_summary_; }

  /// The projection string the form currently describes — generated from the
  /// origin spins in origin mode, taken verbatim in custom mode.
  [[nodiscard]] std::string form_projection() const;

private:
  void build_ui();
  void sync_mode();
  void render_workspace();

  Document& document_;
  SelectionModel& selection_;

  QRadioButton* origin_mode_ = nullptr;
  QRadioButton* custom_mode_ = nullptr;
  QDoubleSpinBox* latitude_ = nullptr;
  QDoubleSpinBox* longitude_ = nullptr;
  QPlainTextEdit* projection_text_ = nullptr;
  QDoubleSpinBox* offset_x_ = nullptr;
  QDoubleSpinBox* offset_y_ = nullptr;
  QDoubleSpinBox* offset_z_ = nullptr;
  QDoubleSpinBox* offset_hdg_ = nullptr;
  QPushButton* clear_offset_ = nullptr;
  QLabel* summary_ = nullptr;
  QLabel* workspace_summary_ = nullptr;

  /// True while refresh() is writing the controls, so the change handlers do
  /// not read a half-populated form back out as a user edit.
  bool loading_ = false;
};

} // namespace roadmaker::editor
