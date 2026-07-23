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

// Sign tool (p4-s9, issue #230). A placement-only sibling of the Signal tool for
// road signs: a click (or a drag + release) places ONE sign as a single
// add_signal command, then selects it so the Attributes pane edits its face
// text. It reuses the signal_placement.hpp helpers (nearest_signal_station,
// make_signal, next_signal_odr_id), so it snaps and mints ids identically to the
// Library drop and the Signal tool.
//
// Unlike the Signal tool it does NOT target junctions or apply templates — signs
// are standalone road-relative entities. When the Library selection is a sign
// asset it places that; otherwise it DEFAULTS to a StVO 310 text plate (empty
// space places nothing — OpenDRIVE has no world-placed signal), so the tool is
// instantly usable with no Library interaction.
//
// M2 drag rule: a plain click places on release; a drag opens a preview session
// and commits exactly ONE command on release. Esc cancels. Headless by
// construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/id.hpp"

#include <functional>
#include <optional>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp" // RoadStation
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class SignTool : public Tool {
  Q_OBJECT

public:
  SignTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The Library's selected item. When it names a sign asset a click places it;
  /// otherwise the tool falls back to the default text sign.
  void set_params_provider(std::function<LibraryItem()> provider);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] PreviewGeometry preview() const override;
  [[nodiscard]] QString instruction() const override;

private:
  /// An armed placement: the press position and the station it snapped to.
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    std::optional<RoadStation> placement;
    bool dragging = false;
  };

  /// The Library `signal` tag to place: the selected item's tag when it is a
  /// sign asset, otherwise "sign_text" (the default StVO 310 plate).
  [[nodiscard]] QString current_tag() const;

  void place_sign(const RoadStation& placement);
  void update_drag(const ToolEvent& event);
  void reset_all();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;

  std::optional<PressState> press_;
  /// The ghost pose while hovering (not dragging).
  std::optional<RoadStation> hover_;
};

} // namespace roadmaker::editor
