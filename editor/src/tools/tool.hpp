#pragma once

#include <QObject>
#include <QString>
#include <optional>
#include <vector>

#include "viewport/picking.hpp"

namespace roadmaker::editor {

// M2 editing-tool state machine (skeleton — see docs/m2/01_editing_framework.md
// §4). Tools are viewport-agnostic controllers: they receive abstract events
// (world-space cursor, picks, modifiers) translated by ViewportWidget and act
// on the network exclusively through Document commands, so their interaction
// logic runs headless under gtest.

enum class ToolId {
  Select,
  CreateRoad,
  EditNodes,
  LaneProfile,
  Elevation,
  CreateJunction,
  Delete,
};

struct ToolEvent {
  // Cursor ray intersected with the z=0 ground plane, kernel frame (meters).
  double world_x = 0.0;
  double world_y = 0.0;
  std::optional<PickHit> pick;
  Qt::MouseButtons buttons = Qt::NoButton;
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

// Overlay geometry in the kernel frame; the viewport uploads it through the
// Renderer's line-primitive path. xyz triples; line positions are consumed
// pairwise as segments.
struct PreviewGeometry {
  std::vector<double> line_positions;
  std::vector<double> point_positions;

  [[nodiscard]] bool empty() const { return line_positions.empty() && point_positions.empty(); }
};

class Tool : public QObject {
  Q_OBJECT

public:
  explicit Tool(QObject* parent = nullptr);
  ~Tool() override;

  // Called by ToolManager on switch; implementations reset their state here.
  virtual void activate() {}

  virtual void deactivate() {}

  // Return true when the event was consumed (the viewport then skips its own
  // handling, e.g. camera navigation on the same button).
  [[nodiscard]] virtual bool mouse_press(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool mouse_move(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool mouse_release(const ToolEvent&) { return false; }

  // Qt double-click sequence is press, release, double-click, release — the
  // pair's first press is delivered normally, so a tool placing points on
  // press sees one point plus this commit gesture (Create Road, 02 §2).
  [[nodiscard]] virtual bool mouse_double_click(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool key_press(int key, Qt::KeyboardModifiers) {
    return static_cast<void>(key), false;
  }

  [[nodiscard]] virtual PreviewGeometry preview() const { return {}; }

signals:
  void preview_changed();
  void status_message(const QString& text);
};

} // namespace roadmaker::editor
