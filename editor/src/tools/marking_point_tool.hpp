#pragma once

// Marking Point tool (p3-s4, issue #223). Click a lane: an arrow stencil
// carrying the selected Library asset's glyph places at the picked station,
// oriented to the lane's travel direction, as ONE undo entry (GW-5 step 6). A
// drag on an already-placed stencil re-locates it along its road through a
// preview session committed as exactly ONE edit::move_object on release (no
// mergeWith — the M2 drag rule). A ghost glyph previews the hovered/dragged
// pose. Click-to-act like the Crosswalk tool; placement funnels through
// stencil_placement so the interactive tool and the Library drop land the same
// object. Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/id.hpp"

#include <functional>
#include <optional>

#include "document/library_manifest.hpp"
#include "document/stencil_placement.hpp"
#include "render/material_catalog.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class MarkingPointTool : public Tool {
  Q_OBJECT

public:
  MarkingPointTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The stencil asset the next click places. MainWindow wires this to the
  /// merged Library's default Stencil asset. An incompatible/unset item makes a
  /// click toast rather than place.
  void set_params_provider(std::function<LibraryItem()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The ghost arrow glyph at the hovered (or dragged) lane pose, else empty.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

private:
  /// LMB held: either a click (place a new stencil) or the start of a drag on a
  /// stencil already placed under the cursor.
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    std::optional<ObjectId> object; ///< set when the press landed on a stencil
    RoadId object_road;             ///< that stencil's owning road
  };

  /// An in-flight stencil drag: the object, its owning road, and whether the
  /// cursor has left the road (so the hint fires on the transition only).
  struct DragState {
    ObjectId object;
    RoadId road;
    bool off_road = false;
  };

  [[nodiscard]] LibraryItem current_item() const;
  void place_stencil(double world_x, double world_y);
  void begin_drag(ObjectId object, RoadId road);
  void update_drag(double world_x, double world_y);
  void select_object(RoadId road, ObjectId object);
  void reset();

  Document& document_;
  SelectionModel& selection_;
  MaterialCatalog materials_;
  std::function<LibraryItem()> params_provider_;
  std::optional<PressState> press_;
  std::optional<DragState> drag_;
  std::optional<StencilPose> hover_; ///< the ghost pose while hovering (not dragging)
};

} // namespace roadmaker::editor
