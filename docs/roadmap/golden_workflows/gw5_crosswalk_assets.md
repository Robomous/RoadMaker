# GW-5 — Parametric crosswalk assets

*Accepts the P3/P6 asset model for crosswalks: crosswalks are parametric
assets authored in the Library Browser and consumed by the marking tools.*

**Status: draft** — steps are refined as the owning pillar sprints land.

> **Sprint coverage (p3-s2, #221):** the crosswalk asset model, Library entry,
> Attributes-pane editor, default-vs-override propagation, and OpenDRIVE
> round-trip land here (steps 1–4, 7, 9–10). The *placing* tools — the
> Crosswalk & Stop Line tool (steps 5, 8) and the Marking Curve tool (step 6) —
> are p3-s3/p3-s4; until then, instances come from the existing junction
> "Add crosswalks to all arms" generator, which authors them linked to the
> default crosswalk asset.

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
   asset's parameters.
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
    crosswalks as OpenDRIVE objects/markings and validates.

## Pass criteria

- Every step's expected result holds; zero crashes.
- Default-vs-override semantics behave exactly as steps 7–9 describe.
- The asset lives in the project's shared asset folder and is reusable
  from a second scene in the same project.

## Results

| Date | OS | Commit | Result | Notes |
|---|---|---|---|---|
| — | — | — | — | no runs yet |
