# Phase 3 — Props (trees) end-to-end (design notes)

Part of the M3a UI revamp (epic
[#108](https://github.com/Robomous/RoadMaker/issues/108), phase
[#113](https://github.com/Robomous/RoadMaker/issues/113)). Phase 3 makes the
Library able to place **trees**: drag a tree onto a road → an OpenDRIVE
`<object>` on that road → drawn in the viewport, selectable/movable/deletable,
and carried into glTF/USD exports. Built in slices (one PR each):

1. **Assets + prop library** (this doc): the bundled tree meshes + catalogue.
2. **Kernel wiring**: `edit::` object commands, prop geometry in the mesh, and
   exporter emission.
3. **Editor entity model + rendering**: `ObjectId` threaded through
   picking/selection/highlight; instanced viewport draw; select/move/delete.
4. **Placement**: drag-and-drop from the Library with a ghost preview.

## Decisions

### Trees are road-attached only (no world-xy placement)

OpenDRIVE `<object>` elements live under a `<road>` and are located by `s`/`t`
(chapter 13, `asam.net:xodr:...:road.object.*`); the format has no free-floating
world-placed object, and the RoadMaker kernel models objects strictly as
road-relative (`Object{ RoadId road; double s, t, z_offset; ... }`,
`core/include/roadmaker/road/object.hpp`). Phase 3 honours that: a tree snaps to
the nearest road's `(s, t)` within a threshold; a drop with **no road nearby
places nothing and shows a hint toast**. This keeps every placed prop
standards-valid and round-trippable, and avoids inventing a non-standard
world-xy attribute. (Maintainer decision, 2026-07-13.)

### Assets are procedurally authored original work (not fetched art)

The renderer consumes flat-shaded `positions/normals/indices` with one colour
per submesh and has no glTF/OBJ loader. Rather than fetch a third-party CC0 pack
(zip-only distribution that fights the per-file-sha256 fetch pipeline, and needs
a mesh/material parser), the trees are **procedurally authored original work**
(MIT, "original work (this repository)") — the same footing as the custom icon
glyphs. `scripts/gen_prop_meshes.py` builds five low-poly props from parametric
trunks/cones/icosahedral crowns and emits two consistent forms:

- `assets/library/props/<id>.obj` (+ `.mtl`) — an inspectable reference export
  (openable in Blender/MeshLab); purely provenance, nothing loads it at runtime.
- `core/src/assets/prop_meshes.gen.cpp` — the embedded, flat-shaded mesh table
  the kernel compiles in and exposes through `roadmaker::props::model(id)`
  (`core/include/roadmaker/assets/prop_library.hpp`).

This is deterministic, reproducible in CI (no external download), license-clean,
and adds no runtime dependency. (Maintainer decision, 2026-07-13.)

### One canonical mesh for viewport and export

`roadmaker::props` is the single source of truth for prop geometry so a tree
looks identical wherever it is drawn or exported. The kernel mesh builder places
the model at the object's world pose (road reference line + elevation, `s/t/hdg`,
`z_offset`), the editor renderer draws that geometry, and the glTF/USD exporters
emit an instance per object from the same table — no divergent art paths.

### The bundled props

| id | label | OpenDRIVE type | ~height | ~radius |
|---|---|---|---|---|
| `tree_pine` | Pine tree | `tree` | 4.2 m | 1.2 m |
| `tree_oak` | Oak tree | `tree` | 4.6 m | 1.8 m |
| `tree_birch` | Birch tree | `tree` | 4.7 m | 1.0 m |
| `tree_poplar` | Poplar tree | `tree` | 6.0 m | 0.85 m |
| `shrub` | Shrub | `vegetation` | 1.1 m | 1.1 m |

Each prop's origin is its base centre (z=0 sits on the road surface); `height`
and `radius` map to the OpenDRIVE object attributes. The Library catalogue
entries (a **Props** category in `assets/library/manifest.json` with
`create: { kind: "tree", model: "<id>" }`) land in the placement slice, together
with the editor's `kind: "tree"` parsing and the drop handler — so the shipped
catalogue never advertises an item the editor cannot yet act on.

![The five bundled low-poly props rendered from their generated meshes](phase3_props.png)

*The five props rendered directly from the generated `assets/library/props/*.obj`
meshes (flat-shaded, trunk + crown submeshes). Viewport/export rendering is
wired in the following slices.*

## Regenerating the props

    python3 scripts/gen_prop_meshes.py   # rewrites the OBJs and the .gen.cpp

CI does not regenerate (the committed `.gen.cpp` is the build input); rerun the
script by hand after changing a tree parameter, and commit both outputs.
