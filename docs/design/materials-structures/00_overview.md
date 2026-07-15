# Materials & Structures overview — v0.7.0

Status: **proposed** — this planning pass runs against the gate in the
[seed](../../roadmap/seeds/materials-structures.md) and epic
[#183](https://github.com/Robomous/RoadMaker/issues/183) ("planning runs after
GS-1 closes"). The maintainer approves it by merging this PR; on approval the
scope **freezes** the way `docs/design/m2/` and `docs/design/m3a/` did — scope
does not grow during implementation, and deviations found while building are
recorded inline as **As-built** notes.

This milestone gives RoadMaker *visual depth*: surfaces that read as real
materials rather than tinted lane types, and built structures that carry a road
over another road. The acceptance artifact is **golden scene GS-4 "Rural
overpass"** ([spec](../../roadmap/golden_scenes/gs4_rural_overpass.md)). Release
target: **v0.7.0**. Gap coverage:
[gap 6](../../roadmap/gap_analysis.md). It is also the **prerequisite for GS-2's
approval** — an imported district is unapprovable without buildings and material
variety, so M3b depends on this landing first.

## Baseline (what M3a shipped)

This milestone builds on the v0.6.0 substrate and does not revisit it:

- **Textured viewport** — `Renderer` interface with a `Material` carrying a base
  colour texture, `uv_scale`, tint, and an `unlit` flag
  (`editor/src/render/renderer.hpp`). GL 3.3 core, all shaders inline in
  `gl_renderer.cpp`.
- **Lighting** — one directional sun + hemisphere ambient, **no shadows, no
  IBL**. `textured_lighting()` / `sober_lighting()` presets;
  `View ▸ Textured Rendering` toggles, sober is default.
- **Surface classification** — `scene_builder.cpp` maps kernel `LaneType` to a
  four-value `SurfaceKind` enum (`Untextured`/`Asphalt`/`Concrete`/`Paint`),
  which `viewport_widget.cpp:135` resolves to a `Material` against two
  Qt-resource textures.
- **Library** — manifest-driven panel (`assets/library/manifest.json`, parsed by
  `editor/src/document/library_manifest.cpp`), drag-and-drop creation, props
  and signals end to end. The schema is versioned and forward-compatible: an
  unknown item kind degrades to `Unknown` rather than breaking the build.
- **Command layer** — every mutation is an `edit::Command`; one `QUndoStack`.
- **Golden-scene machinery** — fixed camera presets, `visual-artifacts` CI
  render, committed baselines, esmini round-trip gate.

## Document map

| Doc | Contents |
|---|---|
| `01_material_system.md` | PBR-lite definition, manifest schema, assignment UX, **the persistence split** (standard `<material>` + `<userData>`, no sidecar) |
| `02_bridge_generator.md` | Bridge v1 scope, Manifold solids, `<bridge>` representation, auto-offer UX, degenerate cases |
| `03_city_props.md` | CC0 kit selection, import path, category layout, GS-2 coverage; sign-text as stretch |
| `04_phases.md` | WS-1…WS-5 with per-phase gates and issue mapping |

## Scope

Ordered kernel → assets → render → editor, per the standing product-parity rule
(nothing renders that the kernel cannot represent and validate first).

1. **Kernel**
   - Promote `<material>` from the **Preserved tier** (today it round-trips as
     an opaque `RawXml` fragment) to the modeled tier: typed `sOffset`,
     `surface`, `friction`, `roughness` on lanes, with rule-id-cited validation.
   - `<bridge>` records within `<objects>`: parse/represent/write/validate.
   - Material assignment as undoable `edit::Command`s.
   - Bridge solid generation via Manifold, driven by a grade-separation query.
2. **Assets**
   - Material sets with albedo + normal + roughness (asphalt ×2 variants,
     concrete, grass upgrade), CC0-first per the
     [asset standard](../../standards/assets.md).
   - CC0 city prop kit (buildings, streetlights).
3. **Render**
   - Normal + roughness sampling in the existing lighting path. **Still no IBL,
     still no shadows.**
   - Per-material map sets loaded from the manifest; fallback to flat colour
     when maps are absent, so old scenes are unaffected.
   - Tangent handling for normal mapping (see risk 1).
4. **Editor**
   - Material library category; assign by drag-onto-surface and via a
     properties dropdown; variants.
   - Bridge generator: auto-offered on detected grade separation, plus manual
     generation from a road-span selection; regeneration on elevation edits.

## Maintainer decisions (locked)

1. **Material persistence: standard `<material>` + `<userData>`, no sidecar.**
   Decided during this planning pass, grounded in the local spec text rather
   than recollection — full split table and citations in `01` §3. The seed
   flagged this as "open question, flag don't decide"; it is decided here
   because the evidence is unambiguous and stalling would block WS-2. **Flagged
   on the epic for maintainer awareness** — the reversible part is the
   `userData` code namespace, not the standard-first principle.
2. **Bridge v1 = deck + abutments/piers + guardrails only**, one guardrail
   style (approved default in the seed, unchanged).
3. **Buildings are Library props (CC0), not procedural generation** (approved
   default in the seed, unchanged).
4. **PBR-lite means albedo + normal + roughness. No IBL. No shadow maps.**
   Consistent with M3a's lighting decision; re-stated here because it is the
   milestone's top scope-creep risk.
5. **Sign-text rendering is stretch**, not committed scope (seed's open
   question 3, resolved to stretch). Mini-spec in `03` §4.

## Non-goals (explicit descopes)

- **Ground elevation / terrain sculpting** — heightmap terrain is **M3b
  (v0.8.0)** per [ADR-0006](../../decisions/0006-terrain-scope.md), which states
  explicitly that terrain is distinct from this milestone. GS-4 proves materials
  and structures on the M3a flat skirt; rolling hills are out of scope.
- **Shadows and image-based lighting** — deferred indefinitely; not a milestone
  gate.
- **Procedural building generation** — Library props only (decision 3).
- **Tunnels** — `<tunnel>` is a sibling of `<bridge>` in the standard and is
  deliberately not in v1.
- **Material authoring UI** — users assign existing materials from the library;
  creating/editing material definitions is manifest work, not an editor feature.
- **CRG / friction simulation semantics** — the `friction`/`roughness`
  attributes are written faithfully but RoadMaker does not simulate them.

## Phase map

| Phase | Issue | Contents | Gate |
|---|---|---|---|
| WS-1 material engine | [#196] | Renderer normal/roughness sampling, material param blocks, manifest-driven map sets, kernel `<material>` promotion | A road renders with flat colour vs PBR-lite side by side; old scenes unchanged; `<material>` round-trips |
| WS-2 material library UX | [#197] | Library category, drag-onto-surface, properties dropdown, variants, assignment commands + persistence | Assign asphalt_worn to a lane, save, reload, still worn; undo works |
| WS-3 bridge generator | [#198] | Grade-separation detection, Manifold solids, `<bridge>` records, auto-offer UX, regeneration | GS-4's overpass generates from a two-road crossing; `.xodr` validates zero-error |
| WS-4 city props | [#199] | CC0 kit import, category layout, licence rows | Props place from the Library; every asset has its ledger row |
| WS-5 GS-4 assembly | [#200] | Scene authoring, baseline, checklist walk | Same acceptance machinery as GS-1: one checklist, gaps filed, baseline committed, esmini clean |

[#196]: https://github.com/Robomous/RoadMaker/issues/196
[#197]: https://github.com/Robomous/RoadMaker/issues/197
[#198]: https://github.com/Robomous/RoadMaker/issues/198
[#199]: https://github.com/Robomous/RoadMaker/issues/199
[#200]: https://github.com/Robomous/RoadMaker/issues/200

### Execution order & dependencies

- **WS-1 is the trunk.** Everything else waits on the material engine: WS-2
  assigns what WS-1 can render, WS-3's deck needs the concrete material, WS-5
  needs all of it.
- **WS-2 after WS-1.** The assignment UX has nothing to assign until the engine
  and the manifest schema exist.
- **WS-3 after WS-1**, parallel-safe with WS-2 — the generator can emit solids
  with a hardcoded material while WS-2 builds the picker.
- **WS-4 is parallel-safe with everything** (asset work, no engine dependency).
- **WS-5 last**, depends on all.
- An executor picking work: take the lowest unblocked WS; every issue body
  carries its own deps and gate.

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **No tangent attribute.** The vertex format is a fixed interleaved stride-8 (position/normal/uv); normal mapping needs a tangent basis. | Certain | Medium | Derive the basis per-fragment from screen-space derivatives of position and uv — no vertex-format change, no re-mesh, works for the planar-UV surfaces this milestone maps. Add a real tangent attribute only if a case demands it. Decided in `01` §5. |
| 2 | **PBR-lite scope creep** (the seed's top risk). "While we're in the shader" invites IBL, shadows, parallax. | High | High | Decision 4 is a hard boundary; WS-1's gate is a before/after of one road, not a beauty contest. Anything beyond albedo+normal+roughness is a new milestone. |
| 3 | **`<material>` promotion breaks round-trip.** It currently round-trips as opaque `RawXml`; promoting it to typed fields risks double-emission or reordering for scenes that already carry one. | Medium | High | A fixture with a hand-authored `<material>` (including attributes we do not model) must round-trip byte-identically. `RawXml` keeps unmodeled attributes; the promotion only claims the four typed ones. `01` §4. |
| 4 | **Manifold is unproven here.** Pinned (3.5.2) and linked but **zero call sites exist** — this is its first consumer. `MANIFOLD_CROSS_SECTION` and `MANIFOLD_PAR` are both OFF. | Medium | Medium | WS-3 opens with a spike: extrude one deck solid and mesh it before designing the rest. If profile extrusion needs `CROSS_SECTION`, enabling it is a `deps.cmake` flag flip, not a new dependency (licence already cleared, Apache-2.0). |
| 5 | **Repo weight.** 3 maps × N materials against a ≤512 KB/map, ≤3 maps/material budget. | Medium | Medium | The budget is exactly sized for diff+nor+rough. Resize/re-encode on fetch as M3a did (512 px, JPEG q85); `check_asset_licenses.py` already gates the ledger. |
| 6 | **Second asphalt variant may not exist as CC0.** GS-4 requires two *visibly distinct* asphalts. | Medium | Low | Preference order per the asset standard: CC0 first, then procedural (tint/roughness variation of one set), then AI-generated — which the AI-asset policy explicitly sanctions for "a variant the libraries lack", with full provenance rows. |
| 7 | **GS-4's asset column is a guess.** It names ambientCG; M3a actually shipped Poly Haven. | Certain | Low | Per the asset standard a doc mapping is "a candidate, not a fact, until the manifest row records the verification". Verify at fetch time and update GS-4's column then. |

## Standards references — mandatory usage

Read before implementing, from the local copies in `.claude/references/asam/`
(never from memory), citing rule ids as `asam.net:xodr:<ver>:<rule>`:

- **Lane material** — OpenDRIVE 1.9.0 §11.8.2 (`t_road_lanes_laneSection_lr_lane_material`).
  Rules in play: `asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material`
  (the centre lane shall have no material elements) and
  `asam.net:xodr:1.4.0:road.lane.material.elem_asc_order` (material elements in
  ascending s-order). A material element stays valid until the next one starts
  or the lane section ends.
- **Additional data** — §7.2 `<userData>`, `t_userData` (`code` required, `value`
  optional). The chapter names *"different road textures"* as its example use.
- **Bridges** — §13.12 `<bridge>` within `<objects>` (`t_road_objects_bridge`);
  required `length` and `type` (`e_bridgeType`: concrete/steel/wood/brick);
  `asam.net:xodr:1.7.0:road.object.bridges.define_type` (bridges may be
  restricted to lanes via `<laneValidity>`). Bridges are valid for a road's
  complete cross-section unless a lane-validity record narrows them.
- **Road-mark material** — §11's `roadMark` attribute table: `material`, a
  user-defined identifier defaulting to `"standard"`.

**Version handling (1.8.1 vs 1.9.0).** The lane-material element is **identical**
in both — same UML class, same four attributes, same rules; only the chapter
number moved (**1.8.1 §11.7.2 → 1.9.0 §11.8.2**). No version-conditional code is
needed for `<material>`; any code comment citing it must name both chapters.
`<bridge>` and `<userData>` are likewise unchanged between the two.
