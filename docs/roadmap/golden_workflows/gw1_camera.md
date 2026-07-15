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

1. [ ] Open the empty scene. **Expected:** the camera orbits a pivot
   point of interest 1.5 m above the world origin.
2. [ ] Hold `⌥ (Alt)` + left-drag. **Expected:** the camera rotates
   around the pivot (polar orbit); the pivot stays fixed on screen.
3. [ ] Hold `⌥ (Alt)` + right-drag, then scroll the wheel. **Expected:**
   both zoom toward/away from the pivot.
4. [ ] Zoom continuously toward the pivot without stopping. **Expected:**
   the camera **pushes past the pivot** — the pivot relocates forward and
   travel continues, with no dead stop at the pivot.
5. [ ] Hold `⌥ (Alt)` + left-drag + right-drag together, and separately
   middle-drag. **Expected:** both pan the camera and pivot together,
   1:1 with the ground under the cursor.
6. [ ] Hold `⌥ (Alt)` + `⇧ Shift` + left-drag + right-drag. **Expected:**
   the pivot moves vertically; subsequent orbits use the new height.
7. [ ] Open the junction scene, select a road, press `F`. **Expected:**
   the camera frames the selection.
8. [ ] Clear the selection, press `F`. **Expected:** the camera frames
   all scene content, preserving the current viewing angle.
9. [ ] In the empty scene, press `F`. **Expected:** the camera returns to
   the origin pivot.
10. [ ] Hover the cursor over a road far from center, press `V`.
    **Expected:** the camera frames on the point under the cursor; the
    pivot moves there.
11. [ ] Press `O`, then `P`. **Expected:** orthographic projection, then
    back to perspective, with no jump in the framed content.
12. [ ] Press numpad `8`, `2`, `4`, `6`. **Expected:** the view snaps to
    look from north, south, west, east respectively.
13. [ ] Press numpad `5`. **Expected:** top-down view, aligned with north
    up.
14. [ ] Repeat steps 2–6 immediately after each framing/cardinal action.
    **Expected:** orbit/zoom/pan behave identically around the new pivot.

## Pass criteria

- Every step's expected result holds on the platform under test.
- Zero crashes, zero rendering corruption during the run.
- Every binding is discoverable in the documented shortcut map.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
