# GW-1 — Camera & navigation

*Accepts the P1 camera model: every navigation behavior below must work in
RoadMaker with the documented bindings.*

**Status: draft** — steps are refined as the P1 sprints land; expected
results stay behavior-exact, bindings may gain platform notes.

## Purpose

Verify the orbit-pivot camera model end-to-end: rotation, zoom with
push-past-pivot, panning, pivot control, framing, projection toggle, and
cardinal views. Every other workflow assumes these behaviors.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- A scene with at least two roads and a junction, plus one empty scene.
- A keyboard with a numeric keypad (or the documented numpad-less
  alternates).

## Steps

Bindings are written macOS-first: `⌥ Option (Alt)`.

**Bindings note.** The Select tool moved from `V` to **`Q`** so that `V` could
become frame-on-cursor (step 10). Cardinal views (steps 12–13) are bound to the
numpad digits, with the **top-row digits `8`/`2`/`4`/`6`/`5` as the
numpad-less alternates** — the "or the documented numpad-less alternates" in
the preconditions. Everything below is also reachable from the **View** menu;
the keys are accelerators, not the only route.

1. [x] Open the empty scene. **Expected:** the camera orbits a pivot
   point of interest 1.5 m above the world origin.
2. [x] Hold `⌥ (Alt)` + left-drag. **Expected:** the camera rotates
   around the pivot (polar orbit); the pivot stays fixed on screen.
3. [x] Hold `⌥ (Alt)` + right-drag, then scroll the wheel. **Expected:**
   both zoom toward/away from the pivot. Dragging **up zooms in**, matching
   the wheel's forward-is-closer.
4. [x] Zoom continuously toward the pivot without stopping. **Expected:**
   the camera **pushes past the pivot** — the pivot relocates forward and
   travel continues, with no dead stop at the pivot.
5. [x] Hold `⌥ (Alt)` + left-drag + right-drag together, and separately
   middle-drag. **Expected:** both pan the camera and pivot together,
   1:1 with the ground under the cursor.
6. [x] Hold `⌥ (Alt)` + `⇧ Shift` + left-drag + right-drag. **Expected:**
   the pivot moves vertically; subsequent orbits use the new height.
7. [x] Open the junction scene, select a road, press `F`. **Expected:**
   the camera frames the selection.
8. [x] Clear the selection, press `F`. **Expected:** the camera frames
   all scene content, preserving the current viewing angle.
9. [x] In the empty scene, press `F`. **Expected:** the camera returns to
   the origin pivot.
10. [x] Hover the cursor over a road far from center, press `V`.
    **Expected:** the camera frames on the point under the cursor; the
    pivot moves there. The **zoom distance is preserved** — the pivot
    moves, the camera does not dolly in.
11. [x] Press `O`, then `P`. **Expected:** orthographic projection, then
    back to perspective, with no jump in the framed content.
12. [x] Press numpad `8`, `2`, `4`, `6` (or top-row `8`/`2`/`4`/`6`).
    **Expected:** the view snaps to look from north, south, west, east
    respectively. Cardinals snap **yaw only** — pitch, zoom distance, and
    the pivot are preserved, so the view re-angles without losing its
    place.
13. [x] Press numpad `5` (or top-row `5`). **Expected:** top-down view,
    aligned with north up. The pitch is **near**-vertical (π/2 − 0.01 ≈
    89.4°), which keeps the look-at basis non-degenerate and is
    indistinguishable on screen.
14. [x] Repeat steps 2–6 immediately after each framing/cardinal action.
    **Expected:** orbit/zoom/pan behave identically around the new pivot.

## Pass criteria

- Every step's expected result holds on the platform under test.
- Zero crashes, zero rendering corruption during the run.
- Every binding is discoverable in the documented shortcut map.

## Results

| Date          | OS    | Commit | Result | Notes |
|---------------|-------|---|---|---|
| July 15, 2026 | MacOS | — | — | no runs yet |
