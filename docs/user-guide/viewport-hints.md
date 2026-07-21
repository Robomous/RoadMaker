# Viewport hints

*The card in the top-left corner of the viewport that tells you what the active
tool expects next — and how to turn it off.*

While a tool is active, its instruction ("Click a road to place the first stop
line…") appears in two places at once: the **status bar** at the bottom of the
window, and a small **hint card** in the viewport's top-left corner. The card
exists because during an interaction your eyes are in the viewport, not on the
status bar. It fades after a few seconds of no change and comes back whenever
the instruction does.

The card is passive: it never takes clicks, and the handles, gizmo, and drag
ghosts are drawn *over* it, so it can never hide something you are dragging.

## Turning it off

**View ▸ Viewport Hints**, or press **H** — a checkbox. It is **on** by default,
and the choice persists across restarts (it is stored with your editor
settings).

With it off, the viewport is clean and the **status bar keeps the same
sentence**, so you never lose the guidance — you only move where you read it.
This is the setting to use when presenting or taking screenshots.

Like the render mode, this is a *view* setting: it is not an edit, it is not on
the undo stack, and it never touches your `.xodr`.

## See also

- [Textured rendering](textured-rendering.md) — the other viewport view setting
- [Keyboard shortcuts](shortcuts.md) — every binding, including `H`
- [Camera & navigation](camera-navigation.md) — moving the view the hints sit in
