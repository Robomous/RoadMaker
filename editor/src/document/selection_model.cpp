#include "document/selection_model.hpp"

#include <algorithm>
#include <iterator>

namespace roadmaker::editor {

SelectionModel::SelectionModel(const Document& document, QObject* parent)
    : QObject(parent), document_(document) {
  connect(&document_, &Document::loaded, this, &SelectionModel::clear);
  // Deletion commands leave entries with dead ids behind; selection is view
  // state (not undoable), so they simply drop out — undoing the deletion
  // does not re-select.
  connect(&document_, &Document::topology_changed, this, &SelectionModel::prune_stale);
  // Object add/delete travels the objects channel, not topology_changed, so a
  // deleted prop must be pruned from the selection here too.
  connect(&document_, &Document::objects_changed, this, &SelectionModel::prune_stale);
}

void SelectionModel::select(const SelectionEntry& entry, SelectMode mode) {
  select_many({&entry, 1}, mode);
}

void SelectionModel::select_many(std::span<const SelectionEntry> entries, SelectMode mode) {
  std::vector<SelectionEntry> next =
      mode == SelectMode::Replace ? std::vector<SelectionEntry>{} : entries_;
  for (const SelectionEntry& entry : entries) {
    if (!is_live(entry)) {
      continue;
    }
    const auto present = std::ranges::find(next, entry);
    if (present != next.end()) {
      if (mode == SelectMode::Toggle) {
        next.erase(present);
        continue;
      }
      next.erase(present); // Replace/Add: re-selecting moves it to the back
    }
    next.push_back(entry);
  }
  set(std::move(next));
}

void SelectionModel::clear() {
  set({});
}

void SelectionModel::prune_stale() {
  std::vector<SelectionEntry> live;
  std::ranges::copy_if(entries_, std::back_inserter(live), [this](const SelectionEntry& entry) {
    return is_live(entry);
  });
  set(std::move(live));
}

bool SelectionModel::contains(const SelectionEntry& entry) const {
  return std::ranges::find(entries_, entry) != entries_.end();
}

std::vector<RoadId> SelectionModel::selected_roads() const {
  std::vector<RoadId> roads;
  for (const SelectionEntry& entry : entries_) {
    if (entry.object.is_valid() || entry.junction.is_valid()) {
      continue; // a prop/junction selection puts no road in play
    }
    if (std::ranges::find(roads, entry.road) == roads.end()) {
      roads.push_back(entry.road);
    }
  }
  return roads;
}

std::vector<JunctionId> SelectionModel::selected_junctions() const {
  std::vector<JunctionId> junctions;
  for (const SelectionEntry& entry : entries_) {
    if (entry.junction.is_valid()) {
      junctions.push_back(entry.junction);
    }
  }
  return junctions;
}

std::vector<ObjectId> SelectionModel::selected_objects() const {
  std::vector<ObjectId> objects;
  for (const SelectionEntry& entry : entries_) {
    if (entry.object.is_valid()) {
      objects.push_back(entry.object);
    }
  }
  return objects;
}

bool SelectionModel::is_live(const SelectionEntry& entry) const {
  if (entry.object.is_valid()) {
    return document_.network().object(entry.object) != nullptr;
  }
  if (entry.junction.is_valid()) {
    return document_.network().junction(entry.junction) != nullptr;
  }
  if (document_.network().road(entry.road) == nullptr) {
    return false;
  }
  return !entry.lane.is_valid() || document_.network().lane(entry.lane) != nullptr;
}

void SelectionModel::set(std::vector<SelectionEntry> entries) {
  if (entries_ == entries) {
    return;
  }
  entries_ = std::move(entries);
  emit selection_changed();
}

} // namespace roadmaker::editor
