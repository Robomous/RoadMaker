#pragma once

// The shortcut map (P1/GW-1 pass criterion "every binding is discoverable in
// the documented shortcut map").
//
// One static table is the single source of truth: Actions binds from it, and
// docs/user-guide/shortcuts.md is RENDERED from it. A gtest compares the render
// against the committed page, so a binding that changes without its
// documentation fails CI instead of quietly drifting — which is exactly what
// happened before, when key letters were hand-copied into tooltips.

#include <QKeySequence>
#include <QString>
#include <span>

namespace roadmaker::editor::shortcuts {

/// Stable identifier for one bound action. Adding a value here without adding
/// a row to the table is a compile-time-visible gap (the table is asserted to
/// be complete).
enum class Id {
  // File
  NewScene,
  Open,
  Save,
  SaveAs,
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
  // View
  FrameSelection,
  FrameCursor,
  ViewPerspective,
  ViewOrthographic,
  ViewNorth,
  ViewSouth,
  ViewWest,
  ViewEast,
  ViewTop,
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
};

/// Every bound action, in documentation order.
[[nodiscard]] std::span<const Entry> table();

/// The row for `id`. Every Id has exactly one row (asserted in the tests).
[[nodiscard]] const Entry& entry(Id id);

/// The key sequences for `id`, primary first — feed straight to
/// QAction::setShortcuts so the alternate is never dropped.
[[nodiscard]] QList<QKeySequence> sequences(Id id);

/// The whole map rendered as the body of docs/user-guide/shortcuts.md.
/// PortableText so the page reads the same on every platform (NativeText would
/// bake in whichever OS generated it).
[[nodiscard]] QString markdown();

} // namespace roadmaker::editor::shortcuts
