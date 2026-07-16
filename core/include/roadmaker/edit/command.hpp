#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"

#include <string_view>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::edit {

/// What a command invalidated. The editor maps this to incremental
/// re-meshing (docs/m2/01_editing_framework.md §5); headless consumers may
/// ignore it.
struct DirtySet {
  /// Roads whose geometry or cross-section changed — need re-mesh.
  std::vector<RoadId> roads;

  /// Junctions whose incoming roads changed — need surface regeneration.
  std::vector<JunctionId> junctions;

  /// Roads whose object layer changed (an object/signal was added, moved, or
  /// removed) — keyed by the owning road so the editor re-anchors props and
  /// object markings via remesh_objects() WITHOUT re-tessellating the road
  /// surface (docs/design/m3a/01 §2.4). First mesh consumer is the marking
  /// pass in phase 2 (#69); phase 4 (#71) reuses it for instanced props.
  std::vector<RoadId> objects;

  /// Roads or junctions were added or removed. Drives the editor's wholesale
  /// mesh re-upload (a partial per-road upload cannot add or drop an item) and
  /// prunes selections that named a now-erased id.
  bool topology = false;

  /// This command already brought `junctions` up to date itself; the editor
  /// must not regenerate them again. Set by the commands that build or tear
  /// down junction structure (create/delete junction, split_road, delete_road)
  /// — a second regeneration would double-work or fight them.
  ///
  /// Default false is the safe direction: a command that forgets the flag gets
  /// a redundant regeneration (slow, correct) rather than a stale junction
  /// (fast, wrong). Kept separate from `topology` deliberately — a lane
  /// appearing is topology AND needs regeneration, which one flag cannot say.
  bool junctions_are_current = false;
};

/// One undoable kernel mutation (docs/m2/01_editing_framework.md §1.1).
///
/// Contract: `apply` then `revert` restores the network to a state whose
/// `write_xodr()` output is byte-identical to the pre-apply output. A failed
/// `apply` leaves the network unchanged (commands validate first, mutate
/// after); `apply` after a failed `apply` is undefined. Commands capture
/// value snapshots — never arena pointers, never UI state.
class RM_API Command {
public:
  Command() = default;
  Command(const Command&) = delete;
  Command& operator=(const Command&) = delete;
  Command(Command&&) = delete;
  Command& operator=(Command&&) = delete;
  virtual ~Command() = default;

  [[nodiscard]] virtual Expected<void> apply(RoadNetwork& network) = 0;
  [[nodiscard]] virtual Expected<void> revert(RoadNetwork& network) = 0;

  /// Human-readable operation name for undo menu text ("Move Waypoint").
  [[nodiscard]] virtual std::string_view name() const = 0;

  /// Valid after a successful apply() or revert().
  [[nodiscard]] virtual DirtySet dirty() const = 0;
};

} // namespace roadmaker::edit
