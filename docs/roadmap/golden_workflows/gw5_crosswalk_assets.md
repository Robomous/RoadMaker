# GW-5 — Parametric crosswalk assets

*Accepts the P3/P6 asset model for crosswalks: crosswalks are parametric
assets authored in the Library Browser and consumed by the marking tools.*

**Status: draft** — steps are refined as the owning pillar sprints land.

> **Sprint coverage (p3-s2, #221):** the crosswalk asset model, Library entry,
> Attributes-pane editor, default-vs-override propagation, and OpenDRIVE
> round-trip land here (steps 1–4, 7, 9–10). The *placing* tools — the
> Crosswalk & Stop Line tool (steps 5, 8) and the Marking Curve tool (step 6) —
> **have since shipped**: p3-s3 (#222, PR #302) and p3-s4 (#223, PR #303),
> both merged 2026-07-18/19, so every step below is executable as written.
> *(Corrected 2026-07-28 — this note previously said "until then, instances
> come from the junction generator", which was stale.)*

## Purpose

Verify that crosswalk assets can be created and edited in the Library with
the full parameter set, that instances consume them via the Crosswalk &
Stop Line tool and the Marking Curve tool, and that instances follow the
asset's defaults unless individually overridden.

## Preconditions

- A dev build of `roadmaker-editor` at the commit under test.
- A scene with a signalized four-arm junction (GW-4 output is fine).
- The starter material library with at least two marking materials.

## Steps

1. [ ] In the Library Browser, create a new **crosswalk asset**.
   **Expected:** it appears in the Library with a preview and an editable
   parameter set in the Attributes pane.
2. [ ] Inspect the parameters. **Expected:** the asset exposes **Width**,
   **Border Width**, **Dash Length**, **Dash Gap**, **Default Material**,
   and a segmentation **Category**.
3. [ ] Set Dash Length to 0. **Expected:** the preview shows a solid
   (unbroken) crosswalk band.
4. [ ] Set a non-zero Dash Length and Dash Gap. **Expected:** the preview
   shows the striped pattern with those measurements.
5. [ ] With the Crosswalk & Stop Line tool, place two instances of the
   asset at two junction approaches. **Expected:** both render with the
   asset's parameters, and one Ctrl+Z removes the crosswalk together with
   its arm's stop-line link (the stop line itself is derived and stays —
   see step 5b).
5b. [ ] With the Stop Line tool (⇧O), click one of those approaches' stop
   lines and drag it 5 m further from the junction, then press **F**.
   **Expected:** the band slides along the arm as one undo step, the
   Attributes pane's **Distance** tracks it, F flips it to span the
   outgoing lanes, and **Reset to default** returns it to 4 m.
6. [ ] With the Marking Curve tool, draw a free-form crossing using the
   same asset. **Expected:** the marking follows the drawn curve with the
   asset's pattern.
7. [ ] Edit the asset's **Width** in the Library. **Expected:** all three
   instances update.
8. [ ] Override the **material** on one instance (drag a different
   marking material onto that instance's material slot). **Expected:**
   only that instance changes.
9. [ ] Change the asset's **Default Material**. **Expected:** the two
   non-overridden instances update; the overridden instance keeps its
   override.
10. [ ] Save and reload. **Expected:** asset parameters, instances, and
    the override round-trip; the exported `.xodr` represents the
    crosswalks as OpenDRIVE objects/markings and validates. The stop-line
    distance and flip from step 5b survive too, and every arm's line is
    exported as a plain `<object type="roadMark" subtype="signalLines">`
    that a third-party viewer draws without any RoadMaker knowledge.

### Importing your own prop (p6-s8, #322)

11. [ ] With a project open, choose **File ▸ Import ▸ Asset (image)…** and pick a
    `.glb` or `.gltf`. **Expected:** the same Import Asset dialog as a texture;
    the file filter offers models as well as images.
12. [ ] Import it. **Expected:** a toast names the asset; a new row appears in the
    Library's **Props** category with a themed glyph rather than a preview
    (render-to-thumbnail is out of scope — [#509](https://github.com/Robomous/RoadMaker/issues/509)).
    If the model was textured, the Diagnostics panel says its texture was
    flattened to an average colour and cites
    [#507](https://github.com/Robomous/RoadMaker/issues/507).
13. [ ] Select the row and place instances with **Prop Point**. **Expected:** it
    places, picks, frames and shows in the Attributes pane exactly as a bundled
    prop does — it is not a special case anywhere in the tool.
14. [ ] Add it to a **Prop Set** alongside a bundled model and scatter with
    **Prop Curve**. **Expected:** the weighted draw includes it.
15. [ ] Set a per-instance **Height** in the Attributes pane. **Expected:** the
    instance rescales by declared-height ÷ authored-height, the same rule a
    bundled prop obeys ([#335](https://github.com/Robomous/RoadMaker/issues/335)).
16. [ ] Export to glTF and USD. **Expected:** every instance is drawn, and each
    part carries a flat material named after its glTF material.
17. [ ] Close the project and reopen it. **Expected:** the prop is still in the
    Library, still placed in the scene, and still draws.
18. [ ] Delete the model file from `assets/props/` and reopen the project.
    **Expected:** the project still opens; the Diagnostics panel names the asset
    that will not draw. **Not** a crash and not a refused library.
19. [ ] Open the same scene **without** its project. **Expected:** the
    `<object>` records round-trip untouched, but the props do not draw — and
    today RoadMaker says nothing about it, which is the gap
    [#508](https://github.com/Robomous/RoadMaker/issues/508) tracks.

## Pass criteria

- Every step's expected result holds; zero crashes.
- Default-vs-override semantics behave exactly as steps 7–9 describe.
- The asset lives in the project's shared asset folder and is reusable
  from a second scene in the same project.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
