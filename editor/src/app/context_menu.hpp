#pragma once

// Right-click context menus (M3a). A headless DESCRIPTOR builder plus a thin
// QMenu assembly. build_context_menu() returns a list of MenuItem (text,
// enabled, separator, an invoke() closure) for a MenuContext — the road/lane,
// station, node, or junction under the cursor — so the whole menu logic is
// unit-testable without a QMenu. assemble_context_menu() wraps that list into a
// QMenu for the viewport and the scene tree to popup. The single source of
// truth the guided tour (#114) will also consume.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/id.hpp"

#include <QString>
#include <functional>
#include <optional>
#include <vector>

#include "viewport/picking.hpp"

class QMenu;
class QWidget;

namespace roadmaker::editor {

class Document;
class SelectionModel;
class Actions;

/// What the cursor is over when the menu opens. Node wins over body; an empty
/// context (no pick, no node, no junction) yields the scene-wide menu.
struct MenuContext {
  std::optional<PickHit> pick;        ///< road/lane under the cursor
  std::optional<double> station;      ///< s on the picked road at the cursor
  std::optional<WaypointHit> node;    ///< authoring node under the cursor
  std::optional<JunctionId> junction; ///< junction under the cursor
};

/// One menu entry. `separator` items ignore text/invoke. A disabled item shows
/// but does nothing (its `invoke` is not called).
struct MenuItem {
  QString text;
  bool enabled = true;
  bool separator = false;
  std::function<void()> invoke;
};

/// Collaborators the item closures act through — registry actions wrap
/// action->trigger(); parametrized ops push commands on the document.
struct ContextMenuDeps {
  Document& document;
  SelectionModel& selection;
  Actions& actions;

  /// Resolves the crosswalk asset "Add crosswalks to all arms" places (p3-s2):
  /// the first Kind::Crosswalk in the merged Library, else the built-in
  /// crosswalk.zebra defaults. Empty (the default) uses plain CrosswalkParams —
  /// the pre-p3-s2 behaviour and what headless tests without a Library get.
  std::function<edit::CrosswalkParams()> default_crosswalk_params;
};

/// The menu descriptor for `context`. Pure logic (no QMenu) — the seam the
/// tests drive: assert on items/enabled and that invoke() lands the command.
/// Maps a viewport pick onto the MenuContext fields build_context_menu keys off:
/// a road/lane hit carries the station (s at the hit point), while a junction
/// floor reports its JunctionId with road/lane invalid. Split out of the GL
/// widget's right-click handler so the wiring has a test seam — the pure menu
/// builder was covered while the code feeding it was not, which is how the
/// junction menu stayed unreachable.
[[nodiscard]] MenuContext menu_context_for_pick(const RoadNetwork& network,
                                                const std::optional<PickHit>& hit);

[[nodiscard]] std::vector<MenuItem> build_context_menu(const MenuContext& context,
                                                       ContextMenuDeps& deps);

/// Assembles build_context_menu() into a QMenu owned by `parent` (deleted on
/// close). Returns nullptr when the descriptor is empty.
[[nodiscard]] QMenu*
assemble_context_menu(const MenuContext& context, ContextMenuDeps& deps, QWidget* parent);

} // namespace roadmaker::editor
