#pragma once

// Single source of truth for the current selection. Every selection flow
// (scene tree, viewport picking, diagnostics navigation) goes through this
// class — widgets never notify each other directly.

#include "roadmaker/road/id.hpp"

#include <QObject>
#include <span>
#include <vector>

#include "document/document.hpp"

namespace roadmaker::editor {

/// One selected entity: a whole road (lane invalid) or a lane on a road.
struct SelectionEntry {
  RoadId road;
  LaneId lane; // invalid = road-level selection

  friend bool operator==(const SelectionEntry&, const SelectionEntry&) = default;
};

/// How a select() call combines with the existing selection.
enum class SelectMode {
  Replace, ///< The entries become the whole selection (plain click).
  Toggle,  ///< Present entries leave, absent ones join (Ctrl+click).
  Add,     ///< Absent entries join; present ones become most-recent (Shift).
};

class SelectionModel : public QObject {
  Q_OBJECT

public:
  /// Clears itself automatically when `document` is reloaded: generational
  /// IDs are only stale-safe within one RoadNetwork instance — after a load,
  /// an old id can alias a fresh entity, so lookups alone cannot detect it.
  explicit SelectionModel(const Document& document, QObject* parent = nullptr);

  /// Applies one entry. Invalid/stale entries are dropped, so a Replace
  /// select of a stale entry clears the selection.
  void select(const SelectionEntry& entry, SelectMode mode = SelectMode::Replace);

  /// Applies a batch (rubber band, tree multi-select) as ONE change: at most
  /// one selection_changed() fires. Duplicates and stale entries are dropped.
  void select_many(std::span<const SelectionEntry> entries, SelectMode mode = SelectMode::Replace);

  void clear();

  /// Ordered oldest → most recent; re-selecting an entry moves it to the
  /// back. No duplicates.
  [[nodiscard]] const std::vector<SelectionEntry>& entries() const { return entries_; }

  /// The most recently selected entry (drives the Properties panel);
  /// default-constructed (invalid road) when the selection is empty.
  [[nodiscard]] SelectionEntry primary() const {
    return entries_.empty() ? SelectionEntry{} : entries_.back();
  }

  [[nodiscard]] bool contains(const SelectionEntry& entry) const;

  [[nodiscard]] bool empty() const { return entries_.empty(); }

  /// Roads present in the selection, in selection order — road-level and
  /// lane entries of the same road collapse to one. The road set the editing
  /// tools operate on (node handles show on selected roads, 02 §1/§3).
  [[nodiscard]] std::vector<RoadId> selected_roads() const;

signals:
  /// Emitted only when the selection actually changes. Carries no payload —
  /// listeners pull entries()/primary().
  void selection_changed();

private:
  /// True for entries whose ids resolve in the current network.
  [[nodiscard]] bool is_live(const SelectionEntry& entry) const;

  void set(std::vector<SelectionEntry> entries);

  const Document& document_;
  std::vector<SelectionEntry> entries_;
};

} // namespace roadmaker::editor
