#include "document/selection_model.hpp"

namespace roadmaker::editor {

SelectionModel::SelectionModel(const Document& document, QObject* parent)
    : QObject(parent), document_(document) {
  connect(&document_, &Document::loaded, this, &SelectionModel::clear);
}

void SelectionModel::select_road(RoadId road) {
  if (document_.network().road(road) == nullptr) {
    clear();
    return;
  }
  set(road, {});
}

void SelectionModel::select_lane(RoadId road, LaneId lane) {
  if (document_.network().road(road) == nullptr || document_.network().lane(lane) == nullptr) {
    clear();
    return;
  }
  set(road, lane);
}

void SelectionModel::clear() {
  set({}, {});
}

void SelectionModel::set(RoadId road, LaneId lane) {
  if (road_ == road && lane_ == lane) {
    return;
  }
  road_ = road;
  lane_ = lane;
  emit selection_changed(road_, lane_);
}

} // namespace roadmaker::editor
