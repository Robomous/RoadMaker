# M3a textured rendering — per-material & procedural-vs-texture decisions

As-built companion to [`04_render.md`](04_render.md): the concrete
texture-vs-procedural choice for each GS-1 surface class, recorded as it lands
(the design doc mandates recording these per `03_assets.md` and `00` risk 3).
The render-mode framing (Textured default / Sober toggle, hemisphere +
directional lighting, no shadows) is in `04_render.md` §7–§9.

## Decision table

| Surface class | Approach | Status | Rationale |
|---|---|---|---|
| **Grass ground** | **Procedural** (value-noise grass shader) | ✅ landed (WS-D D2) | Reads well without an asset; infinite camera-following plane melts into the sky horizon, so no fetch, no tiling seam, no license row. `03_assets.md` §1 explicitly keeps grass "procedural-first". |
| **Asphalt** (driving/shoulder/junction floor) | **Texture** (Poly Haven `asphalt_02`, CC0) | ✅ landed (WS-A A2) | A tiled photo texture reads better than procedural for close-up road grain. 512² JPEG (87 KB), 4 m tile via `Material::uv_scale`. Source was ambientCG; Poly Haven chosen because it serves direct single-file CC0 downloads that fit the fetch pipeline. |
| **Concrete** (sidewalk / curb / border) | **Texture** (Poly Haven `brushed_concrete`, CC0) | ✅ landed (WS-A A2) | Same rationale; 512² JPEG (51 KB). `surface_for()` maps Sidewalk/Curb/Border → Concrete, everything else paved → Asphalt. |
| **Lane-mark paint** (crosswalk/arrow/stop-line/dual-yellow) | **Procedural** (bright **unlit** paint material) | ✅ landed (WS-A A2) | Marks are geometry, not textures (M1 rule). `SurfaceKind::Paint` → `Material::unlit` so marks read as bright flat paint, not shaded. Z-fighting handled by the existing `kMarkingLift`; a polygon-offset refinement is a follow-up if grazing angles need it. |
| Sky | Procedural gradient | ✅ (pre-existing, driven by `Environment` since D1) | Reads fine; HDRI only if it doesn't (`04_render.md` §2). |

Update this table as A2 / WS-Assets land each material.

## Procedural ground (WS-D, D2) — as-built

- **Geometry:** a single camera-following quad (`kExtent = 6000 m`) at world
  height `ground_base_z(SceneBounds)` = network floor (`bounds.lo[2]`) − 5 cm,
  so a road/junction surface sitting exactly at the floor draws over the opaque
  ground without z-fighting. Not a kernel mesh, never exported.
- **Shading:** two octaves of value noise (~1 m and ~5 m) mottle the grass
  colour; lit as a flat +Z surface by the scene `Environment` (hemisphere sky
  ambient + directional sun), matching the mesh shader for an up-facing normal.
  It fades to the sky-horizon colour with distance (scaled by eye height) so the
  infinite plane has no hard edge.
- **Pass order / depth:** drawn after the sky+grid backdrop and **before** the
  mesh loop, opaque with depth test **and** write on (unlike the reference grid,
  which never writes depth). In Sober mode `set_ground(false, …)` disables it and
  the reference grid remains.
- **Deferred:** the ground is a flat plane at the network floor — it does **not**
  follow road elevation or fit the network footprint with a bounded skirt. A
  bounds-fitted, elevation-following terrain skirt is deferred to the terrain
  scope decision, [ADR-0006 / #83](https://github.com/Robomous/RoadMaker/issues/83)
  (PROPOSED), which must land before M3b import work. GS-1 is flat, so the flat
  plane is exact for the golden scene.

## History

- 2026-07-14 — created with the D2 ground decision; surface-material rows are
  stubs filled by WS-Assets (#70) + WS-A part 2 (A2).
