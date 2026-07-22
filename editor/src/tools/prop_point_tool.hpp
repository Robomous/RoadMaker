#pragma once

// Prop Point tool (p6-s4, issue #238). Click on or beside a road: the selected
// Library prop (a tree/shrub model) places at the snapped station as ONE undo
// entry (GW-2 step 16 — "lands under the cursor and is movable"). A drag on an
// already-placed prop re-locates it along its road through a preview session
// committed as exactly ONE edit::move_object on release (no mergeWith — the M2
// drag rule). A ghost circle of the model radius previews the hovered/dragged
// pose. Placement funnels through prop_placement so the interactive tool and the
// Library tree-drop land the same object. Headless by construction: ToolEvent in,
// commands + PreviewGeometry out.

#include "roadmaker/road/id.hpp"

#include <functional>
#include <optional>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class PropPointTool : public Tool {
  Q_OBJECT

public:
  PropPointTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The prop asset the next click places. MainWindow wires this to the merged
  /// Library's default Tree asset. An incompatible/unset item makes a click toast
  /// rather than place.
  void set_params_provider(std::function<LibraryItem()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The ghost circle at the hovered (or dragged) pose, else empty.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

private:
  /// LMB held: either a click (place a new prop) or the start of a drag on a prop
  /// already placed under the cursor.
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    std::optional<ObjectId> object; ///< set when the press landed on a prop
    RoadId object_road;             ///< that prop's owning road
  };

  /// An in-flight prop drag: the object, its owning road, and whether the cursor
  /// has left the road (so the hint fires on the transition only).
  struct DragState {
    ObjectId object;
    RoadId road;
    bool off_road = false;
  };

  [[nodiscard]] LibraryItem current_item() const;
  void place_prop(double world_x, double world_y);
  void begin_drag(ObjectId object, RoadId road);
  void update_drag(double world_x, double world_y);
  void select_object(RoadId road, ObjectId object);
  void reset();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;
  std::optional<PressState> press_;
  std::optional<DragState> drag_;
  std::optional<RoadStation> hover_; ///< the ghost pose while hovering (not dragging)
  /// Advances per placed instance so a prop set point-placed repeatedly draws a
  /// varied (but reproducible) mix rather than the same model each click (#367).
  unsigned place_seed_ = 0;
};

} // namespace roadmaker::editor
