#pragma once

// Single source of truth for the current selection. Every selection flow
// (scene tree, viewport picking, diagnostics navigation) goes through this
// class — widgets never notify each other directly.

#include "roadmaker/road/id.hpp"

#include <QObject>

#include "document/document.hpp"

namespace roadmaker::editor {

class SelectionModel : public QObject {
  Q_OBJECT

public:
  /// Clears itself automatically when `document` is reloaded: generational
  /// IDs are only stale-safe within one RoadNetwork instance — after a load,
  /// an old id can alias a fresh entity, so lookups alone cannot detect it.
  explicit SelectionModel(const Document& document, QObject* parent = nullptr);

  /// Selects a road (no lane). Invalid/stale ids clear the selection.
  void select_road(RoadId road);

  /// Selects a lane on a road. Invalid/stale ids clear the selection.
  void select_lane(RoadId road, LaneId lane);

  void clear();

  [[nodiscard]] RoadId road() const { return road_; }

  /// Invalid unless a lane is selected.
  [[nodiscard]] LaneId lane() const { return lane_; }

  [[nodiscard]] bool empty() const { return !road_.is_valid(); }

signals:
  /// Emitted only when the selection actually changes.
  void selection_changed(roadmaker::RoadId road, roadmaker::LaneId lane);

private:
  void set(RoadId road, LaneId lane);

  const Document& document_;
  RoadId road_;
  LaneId lane_;
};

} // namespace roadmaker::editor
