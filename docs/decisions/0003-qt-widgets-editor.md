# ADR-0003: Qt 6 Widgets editor, LGPL dynamic-only

*Why the editor is Qt 6 Widgets, and the exact conditions under which the
Apache-2.0 project carries its single LGPL dependency.*

- **Status:** accepted
- **Date:** 2026-07 (Qt migration shipped in v0.2.0)
- **Deciders:** Armando Anaya

## Context

The M1 viewer began on an immediate-mode UI (Dear ImGui) that was adequate
for a debug-style viewer but a poor base for a real editing product: no
native menus/docking/undo framework, no accessibility, no platform look, and
every panel hand-rolled. A desktop-class editor (dockable panels, item
views, undo stack, settings, installers) was originally scheduled as a later
feasibility spike; the strict kernel/editor boundary made it cheap enough to
pull forward, and the migration landed as v0.2.0 without touching `core/` or
`python/`.

Qt is LGPLv3, and RoadMaker is Apache-2.0 with a hard no-GPL/no-LGPL dependency
policy — so adopting Qt required an explicit, tightly scoped exception.

## Decision

The editor is **Qt 6 Widgets** (no QML), and Qt is the **single sanctioned
LGPL dependency**, under hard conditions:

- **Dynamic linking only.** Never a static Qt build.
- **Editor targets only.** `core/` and `python/` never include a Qt header
  or link a Qt library; the kernel and wheels stay pure Apache-2.0.
- **Never vendored, never modified, never FetchContent.** Qt is provisioned
  by `scripts/setup_qt.py` (aqtinstall) into the gitignored `./.qt/`; the
  version pin lives in `cmake/QtVersion.cmake`.
- **Relink obligation honored in every distribution:** bundles ship the
  LGPLv3/GPLv3 texts (`licenses/`), the About dialog and
  `THIRD_PARTY_LICENSES.md` state that users may replace the Qt libraries,
  and the deploy tools keep Qt as separate shared libraries.

Any other LGPL candidate still requires explicit maintainer approval.

## Consequences

- The editor gets native menus, docks, item models, `QUndoStack`, QSettings,
  and per-OS deployment tooling (macdeployqt/windeployqt/linuxdeploy) —
  the foundation the editing milestones build on.
- Editor logic must stay headless-testable (offscreen platform), because
  widgets are thin by rule — see [editor architecture](../architecture/editor.md).
- Packaging carries Qt's size and per-OS deployment quirks — see
  [cross-platform standard](../standards/cross-platform.md).
- Adding a Qt module is a deliberate act: it must be added to the
  `setup_qt.py` archive list and the deployment expectations together.

## References

- [Dependency & licensing policy](../standards/dependencies.md) — the Qt
  exception in normative form.
- [Editor architecture](../architecture/editor.md)
