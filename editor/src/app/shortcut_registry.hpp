#pragma once

// The action registry (P1/GW-1 pass criterion "every binding is discoverable in
// the documented shortcut map", extended in p1-s5 to toolbar placement).
//
// One static table is the single source of truth for two independent axes, both
// test-gated:
//   * bindings — Actions binds from it, and docs/user-guide/shortcuts.md is
//     RENDERED from it. A gtest compares the render against the committed page,
//     so a binding that changes without its documentation fails CI instead of
//     quietly drifting — which is exactly what happened before, when key letters
//     were hand-copied into tooltips.
//   * toolbar placement — the two categorized toolbar rows are GENERATED from
//     the `toolbar_group`/`toolbar_order` columns, and toolbar_violations() is
//     the gate that stops a new tool landing uncategorized.
// The axes are deliberately independent: `category` is the documentation
// section, `toolbar_group` the toolbar's taxonomy, and a row may sit in one
// without the other (an unbound row never reaches the page; a non-tool row
// never reaches the toolbar).

#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <span>
#include <vector>

namespace roadmaker::editor::shortcuts {

/// Stable identifier for one registered action. Adding a value here without
/// adding a row to the table is a compile-time-visible gap (the table is
/// asserted to be complete).
///
/// Not every Id carries a key binding: p1-s5 admitted toolbar-only actions
/// (ExportGlb, MergeRoads, AddFromLibrary, ResetCamera) so the toolbar can be
/// generated from one table. Those rows render nothing on the shortcuts page.
/// The values are never persisted, so the enum may be renumbered freely.
enum class Id {
  // File
  NewScene,
  Open,
  Save,
  SaveAs,
  ExportGlb,
  Quit,
  // Edit
  Undo,
  Redo,
  // Tools
  ToolSelect,
  ToolMove,
  ToolCreateRoad,
  ToolEditNodes,
  ToolLaneProfile,
  ToolElevation,
  ToolCreateJunction,
  ToolSplit,
  ToolDelete,
  ToolLaneAdd,
  ToolLaneForm,
  ToolLaneCarve,
  ToolCrosswalk,
  ToolMarkingPoint,
  ToolMarkingCurve,
  ToolPropPoint,
  ToolPropCurve,
  ToolPropSpan,
  ToolPropPolygon,
  ToolCorner,
  ToolStopLine,
  ToolJunctionSpan,
  ToolJunctionSurface,
  ToolManeuver,
  ToolSignal,
  LaneWidthEditor,
  SignalPhaseEditor,
  MergeRoads,
  // View
  AddFromLibrary,
  ResetCamera,
  FrameSelection,
  FrameCursor,
  ViewPerspective,
  ViewOrthographic,
  ViewNorth,
  ViewSouth,
  ViewWest,
  ViewEast,
  ViewTop,
  ViewportHints,
  // Help
  Help,

  /// Count sentinel — always last. Iterating `[0, kIdCount)` is what the tests
  /// use to prove the table and the Id→QAction map cover every value.
  kIdCount,
};

/// Which of the two generated toolbar rows a group lives on.
enum class ToolbarRow : std::uint8_t {
  kNone = 0,      ///< not on a toolbar
  kAuthoring = 1, ///< row 1 — building the network
  kLayers = 2,    ///< row 2 — the scene layers laid over it
};

/// One toolbar group: a named run of buttons, separated from its neighbours.
struct ToolbarGroup {
  const char* name;
  ToolbarRow row;
};

/// One row of the map.
struct Entry {
  Id id;
  const char* category;    ///< the section it is documented under
  const char* description; ///< what it does, for the page

  /// A platform's standard binding (File/Edit conventions). When set it is what
  /// Actions binds, so each OS gets its native keys.
  QKeySequence::StandardKey standard = QKeySequence::UnknownKey;

  /// The binding, as QKeyCombination — NOT int. `Qt::KeypadModifier | Qt::Key_8`
  /// already yields a QKeyCombination, and storing it in an int would go through
  /// QKeyCombination::operator int(), which Qt deprecated in 6.0 (MSVC flags it
  /// and CI's /WX turns that into a build failure). A plain `Qt::Key` converts
  /// implicitly, so unmodified rows still read as `.primary = Qt::Key_Q`.
  QKeyCombination primary = Qt::Key_unknown;

  /// A second binding for the same action — the numpad-less digit on a cardinal.
  /// Qt::Key_unknown = none.
  QKeyCombination alternate = Qt::Key_unknown;

  /// What the PAGE says, for `standard` rows only.
  ///
  /// It cannot be rendered from `standard`: QKeySequence::keyBindings returns a
  /// DIFFERENT list per platform (Redo is Ctrl+Y plus two others on Windows,
  /// Cmd+Shift+Z on macOS) and some entries are empty on some platforms (Quit
  /// has no binding on macOS — the platform menu owns ⌘Q). Generating from it
  /// would make the committed page platform-specific, so the doc gate would
  /// pass on the machine that generated it and fail on the other two. This
  /// field is the one spelling the docs commit to; the app still binds
  /// natively from `standard`.
  const char* documented = "";

  const char* note = ""; ///< platform caveat, or "" — rendered as a Notes cell

  /// The toolbar group this action belongs to, naming a row of
  /// toolbar_groups(); nullptr = not on the toolbar at all (SaveAs, Quit,
  /// Undo/Redo, the projection and cardinal views, Help — all menu-only).
  ///
  /// The row is NOT stored per entry: it is derived from the group, so the
  /// taxonomy has exactly one place to drift and exactly one test to gate it.
  const char* toolbar_group = nullptr;

  /// Position within the group, ascending. Assigned in tens so a tool can be
  /// slotted between two existing ones without renumbering. It is not
  /// redundant with table order — the table is in DOCUMENTATION order, which
  /// differs (Elevation is documented before Create Junction but sits after
  /// Corner on the toolbar). Ties keep table order (the sort is stable).
  int toolbar_order = 0;
};

/// Every bound action, in documentation order.
[[nodiscard]] std::span<const Entry> table();

/// The row for `id`. Every Id has exactly one row (asserted in the tests).
[[nodiscard]] const Entry& entry(Id id);

/// The key sequences for `id`, primary first — feed straight to
/// QAction::setShortcuts so the alternate is never dropped. EMPTY for a
/// toolbar-only row (no standard, no primary): setShortcuts([]) is a harmless
/// no-op, which keeps "everything binds from the registry" true for all Ids.
[[nodiscard]] QList<QKeySequence> sequences(Id id);

/// The toolbar taxonomy, in display order within each row. Fixed by issue
/// #317; reserved groups are listed here before they hold anything so the
/// categories are committed rather than invented per sprint.
[[nodiscard]] std::span<const ToolbarGroup> toolbar_groups();

/// One group's generated contents.
struct ToolbarGroupLayout {
  const ToolbarGroup* group;
  std::vector<Id> ids; ///< empty for a reserved group — renders nothing
};

/// The groups on `row` with their actions, both in display order. Reserved
/// groups are present with an empty `ids`.
[[nodiscard]] std::vector<ToolbarGroupLayout> toolbar_layout(ToolbarRow row);

/// The consistency gate: empty when `rows` are well-formed against `groups`,
/// else one message per violation. Checks that (a) every "Tools" row is
/// categorized — this is what stops a new tool landing off-toolbar, (b) every
/// named group exists, (c) no two rows share a (group, order) slot.
///
/// A free function over spans, so the negative test can feed it a deliberately
/// broken fixture instead of having to break the real table.
[[nodiscard]] QStringList toolbar_violations(std::span<const Entry> rows,
                                             std::span<const ToolbarGroup> groups);

/// The whole map rendered as the body of docs/user-guide/shortcuts.md.
/// PortableText so the page reads the same on every platform (NativeText would
/// bake in whichever OS generated it).
[[nodiscard]] QString markdown();

} // namespace roadmaker::editor::shortcuts
