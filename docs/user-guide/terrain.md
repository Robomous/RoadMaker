# Terrain

*Give the whole scene a ground height field — real, shapeable ground the
roads sit in, instead of a flat plate they float over.*

## What terrain is

Until you create it, a scene has **no ground data**. The grass-green plane you
see in the viewport is presentation only: it is drawn at a fixed height and
holds nothing, so a road raised into a hill floats over flat ground with a cliff
at its edge.

**Terrain** replaces that with a real *height field* — a grid of ground heights
covering the scene. Once it exists, the ground is a surface the roads shape:
raise a road and the ground beside it rises to meet the kerb; lower it and the
ground follows down. A scene with no terrain field behaves exactly as before —
creating one is the switch that turns the ground from a picture into data.

## Creating and removing terrain

Terrain lives under **Edit ▸ Terrain**:

- **Create Terrain Field** lays a flat field over the whole network, with a
  margin around it. A flat field is invisible at first — flat ground at height
  zero looks just like the old plate — but the ground is now real, and the next
  road elevation you edit will shape it.
- **Remove Terrain Field** deletes it, returning to the flat presentation
  plane.

Both are ordinary undoable edits (**Ctrl+Z**).

## How the ground follows the road

Where terrain meets a road, RoadMaker blends the ground up (or down) to the
road's edge across a short **skirt band**, so the two always join without a
step — the ground is *cut* where it would sit above the road and *filled* where
it would sit below. Beyond the skirt the ground is the bare field, so a hill in
the road raises only the ground near it, not the whole scene.

To see it: create a terrain field, then open a road's
[elevation profile](elevation.md) and raise a span. The 3D road climbs, and the
ground on both sides climbs with it.

## Saving terrain

The height field is saved in an **ESRI ASCII grid** (`.asc`) file next to your
`.xodr`, and the `.xodr` records a reference to it. Both travel together — keep
the `.asc` beside the scene when you move or share it. If the sidecar is missing
when you open a scene, the roads still load; only the ground comes back flat,
with a note in the diagnostics.

## Not yet in this release

This release gives you the height field and the road coupling. **Raise/lower
brushes** for sculpting the ground directly, and **importing a DEM** (real-world
elevation) are the next terrain sprint — until then, terrain shape comes from
the roads.
