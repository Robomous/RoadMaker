# M3a overview — visual & standards completeness

Status: **approved & frozen** — the maintainer approved this planning pass by
merging PR [#66](https://github.com/Robomous/RoadMaker/pull/66) (2026-07-11);
implementation may proceed phase by phase. This is the frozen-scope design for
milestone **M3a**, written after M2 shipped (v0.3.0) per the gate in the
[M3a seed](../../roadmap/seeds/m3a.md) and epic
[#38](https://github.com/Robomous/RoadMaker/issues/38). The docs are frozen the
way `docs/design/m2/` was: scope does not grow during implementation, and
deviations discovered while building are recorded inline as **As-built** notes
(see `01` §9 for Phase 0's).

> **Rescope (2026-07-12, maintainer pivot — sequencing and release number
> only; the scope below is unchanged).** M3a now **opens with the UI/UX
> revamp epic** ([#108](https://github.com/Robomous/RoadMaker/issues/108),
> phases 0–4: #109, #111–#114 — theme system, viewport feedback, library
> panel + drag-and-drop, props, discoverability); the phases in this
> document follow it as the **standards track** (S1 = #62, S2 = #70,
> S3 = #71, S4 = #72, close-out = #73). Release target shifts to
> **v0.5.0** — the hardening sprint between M2 and M3a took v0.4.0.
> Already delivered ahead of this document's phase map: kernel
> objects/signals/road marks (phases 0–2, #67–#69), autosave & crash
> recovery (#53, hardening PR #99), and the esmini CI gate (#51). The
> minimal read-only library panel (#50, phase 5) is **superseded** by the
> revamp Phase 2 manifest-driven library (#112); the user guide (#52)
> ships with v0.5.0 after a themed-screenshot refresh in revamp Phase 4
> (#114). Strategy record: [product parity](../../standards/product-parity.md)
> / [UI design standard](../../standards/ui-design.md); rescoped
> [roadmap](../../roadmap/roadmap.md) and [seed](../../roadmap/seeds/m3a.md).

M3a turns an authored network from a *correct* road graph into a *road scene
that looks and validates like the real thing*: crosswalks, signals, signs, lane
arrows, and stop lines carry real ASAM OpenDRIVE semantics; a CC0 prop set and
textured, lit viewport make the scene legible; and every exported `.xodr` still
validates clean and survives a headless simulator load. The acceptance artifact
is **golden scene GS-1 "Urban intersection"**
([spec](../../roadmap/golden_scenes/gs1_urban_intersection.md)). Release target:
**v0.5.0** (originally v0.4.0; shifted by the hardening sprint). Gap coverage:
[gap 1 — viewport visual completeness](../../roadmap/gap_analysis.md#gap-1--viewport-visual-completeness).

## Baseline (what M2 shipped)

M3a builds directly on the v0.3.0 substrate and does not revisit it:

- Editing framework: every mutation is an `edit::Command`; one `QUndoStack` on
  `Document`; drags = preview session + one command on release
  ([m2/01](../m2/01_editing_framework.md)).
- Road data model: arena storage + generational IDs; `Road`, `LaneSection`,
  `Lane` (with `RoadMark` on the outer boundary), `Junction`
  ([`core/include/roadmaker/road/`](../../../core/include/roadmaker/road)).
- Parser / version-explicit writer (`core/src/xodr/`), validator with
  `Diagnostic::rule_id` citations (`core/src/xodr/rules.hpp`).
- Mesh builder with incremental `remesh_roads`/`remesh_junctions`
  ([`core/include/roadmaker/mesh/mesh_builder.hpp`](../../../core/include/roadmaker/mesh/mesh_builder.hpp));
  junction 3D blended surfaces ([m2/03](../m2/03_junction_blending.md)).
- Renderer behind the `Renderer` interface; GL only in `editor/src/render/`;
  an optional textured mode scoped (not default) in
  [m2/05](../m2/05_assets.md).
- glTF + OpenUSD (`.usda`) export.

## Document map

| Doc | Contents |
|---|---|
| `01_kernel_objects_signals.md` | OpenDRIVE `<objects>` and `<signals>` — data model (arena-stored, generational ids), parse / write / validate with rule-id-cited diagnostics, GS-1-scoped object/signal classes |
| `02_road_marks.md` | Road-mark completions: multi-line `solid solid` as true dual geometry, road-mark color, stop lines and lane arrows as object markings, mesh generation |
| `03_assets.md` | CC0 prop set (vegetation, poles, signal heads, sign plates), texture-set extension, the committed runtime **library manifest** format |
| `04_render.md` | Textured viewport as default (sober mode kept), terrain skirt + procedural ground, sky/lighting pass (hemisphere + directional, no shadows), instanced prop rendering behind `Renderer` |
| `05_editor_and_docs.md` | Minimal read-only library panel + object/signal placement UX, autosave & crash recovery, esmini round-trip CI gate, minimal user guide |

## Scope

Decomposition follows the standing **kernel → assets → render** rule
([product-parity](../../standards/product-parity.md)); nothing renders that the
kernel cannot represent and validate first.

1. **Kernel — standards data model** (`01`, `02`)
   - `<objects>`: `crosswalk` (outline objects), point props/poles/trees (point
     objects with orientation + `zOffset`), and `<repeat>` records for tree
     lines — parse, represent, write, validate.
   - `<signals>`: dynamic (traffic lights) and static (speed-limit, pedestrian-
     crossing signs) with `s`/`t` placement, `orientation`/`hOffset`, and
     `type`/`subtype`/`country` — parse, represent, write, validate.
   - Road-mark completions: multi-line `solid solid` as **true dual geometry**
     (descoped from M2), road-mark **color**, and object-based **stop lines**
     and **lane arrows**.
   - Junction `<boundary>` completion (deferred from M2 by
     [m2/03](../m2/03_junction_blending.md) §3): counter-clockwise lane/joint
     `<segment>` emission plus **auxiliary boundary roads** where connecting
     roads leave gaps; scope, rule IDs, and acceptance in
     [#62](https://github.com/Robomous/RoadMaker/issues/62).
   - Mesh generation for all of the above: object placeholder / instanced-prop
     anchors, and marking geometry for arrows and stop lines.
2. **Assets — CC0 prop set** (`03`)
   - One primary low-poly kit family (vegetation, poles, signal heads, sign
     plates), verified per-file; asphalt/concrete/grass texture extension.
   - The runtime **library manifest** format (committed here; the M4 browser
     subsumes its model).
3. **Editor / render** (`04`, `05`)
   - Textured viewport becomes the **default** (sober mode kept as a toggle);
     terrain skirt + procedural ground; sky/lighting pass; instanced prop
     rendering behind `Renderer`.
   - Object/signal placement: properties-panel workflow **plus** a minimal
     read-only library panel (flat list + drag-to-place).
   - Autosave and crash recovery.
4. **Cross-cutting quality & docs** (`05`)
   - esmini round-trip smoke-validation CI job (permanent from M3a).
   - Minimal user guide (`docs/user-guide/`) shipped with v0.5.0.

## Maintainer decisions (locked, 2026-07-10)

Recorded in [#34](https://github.com/Robomous/RoadMaker/issues/34) and inline in
the [seed](../../roadmap/seeds/m3a.md):

1. **Signal-catalog depth** — GS-1 signal set only in M3a; country catalogs are
   backlog.
2. **Library panel** — pull a *minimal read-only* panel forward from M4 (flat
   list + drag-to-place, no search/categories). Accepted cost: the runtime
   manifest format is committed in M3a.
3. **Shadows** — deferred. M3a lighting = hemisphere + directional, no shadow
   maps; shadows are M4+ polish.

## Non-goals (explicit descopes)

Deferred beyond M3a (kept out to hold the milestone to GS-1):

- Full country signal catalogs and a signal-catalog data file — M3a hard-codes
  only the GS-1 signal set (decision 1).
- Shadow maps and any global-illumination pass (decision 3).
- Object `<skeleton>` and `<curveLocal>` smooth outlines — M3a represents props
  as point objects + bounding box and crosswalks with straight-segment
  `<cornerRoad>`/`<cornerLocal>` outlines only; skeletons/smooth curves parse
  round-trip-safe but are not authored (`01` §5).
- Full asset **library browser** (search, categories, thumbnails, editing) — M4.
- Signal **controllers**/phases (`<controller>`, junction signal groups) and
  OpenSCENARIO dynamic content — M4/M5.
- Lidar/OSM/GIS import and imagery underlay — M3b.
- Superelevation-aware object framing (props stay reference-line-vertical unless
  `@perpToRoad`) — later render polish.

## Phase map (implementation, after this plan is approved)

One epic tracked by [#38](https://github.com/Robomous/RoadMaker/issues/38)
(milestone **M3a**, label `m3a`); PRs ≤ ~500 lines where feasible; screenshots
in PR descriptions for visual features. Every phase keeps the standing rules:
GoogleTest/pytest with the code; kernel API changes update
`python/src/bindings.cpp` + an example in the same PR; extend the xodr fuzz
corpus for each new element class; sanitizer run before merging geometry/parsing
changes.

| Phase | Issue | Contents | Gate |
|---|---|---|---|
| 0 | [#67](https://github.com/Robomous/RoadMaker/issues/67) | Kernel `<objects>` model + parse/write/validate (crosswalk outline, point objects, `<repeat>`); fuzz-corpus + round-trip tests. **Not here:** edit commands (phase 5) and the object dirty set (phase 2) | `<object>` round-trip stable within `rm::tol`, validator rule-ids cited |
| 1 | [#68](https://github.com/Robomous/RoadMaker/issues/68) | Kernel `<signals>` model + parse/write/validate (dynamic + static, GS-1 set); Python bindings + example. Mirrors the **as-built** Phase 0 patterns (`01` §9), not the original sketches | `<signal>` round-trip stable, GS-1 signal set represented, validator clean |
| 2 | [#69](https://github.com/Robomous/RoadMaker/issues/69) | Road-mark completions: `solid solid` dual geometry + road-mark color; stop lines + lane arrows as object markings (normative subtypes in `02` §3); marking meshes; **`DirtySet::objects`** (`01` §2.4 — defined here with its first mesh consumer) | Marking round-trip + dual-strip mesh; arrows/stop-lines validate |
| 2b | [#62](https://github.com/Robomous/RoadMaker/issues/62) | Junction `<boundary>` segments + auxiliary boundary roads — deferred from M2, scope in the issue. Independent of phases 0–2 (junction-only, builds on M2); must land before phase 6 | `<boundary>` emitted CCW + closed for generated junctions; `close_gap_with_new_roads` warning cleared once the boundary closes; esmini loads without junction warnings |
| 3 | [#70](https://github.com/Robomous/RoadMaker/issues/70) | Assets: CC0 prop kit import + texture extension; library manifest format + loader; `ASSETS_LICENSES.md` rows | Manifest loads; every asset license-verified and recorded; `check_asset_licenses.py` green |
| 4 | [#71](https://github.com/Robomous/RoadMaker/issues/71) | Render: textured mode default + sober toggle; terrain skirt + procedural ground; sky/lighting pass; instanced prop rendering (consumes phase 2's object dirty set; placeholder-box mesh for unresolvable objects is render-side, `04` §3) | Textured lit scene renders headless; sober mode still packages; frame parity test |
| 5 | [#72](https://github.com/Robomous/RoadMaker/issues/72), [#50](https://github.com/Robomous/RoadMaker/issues/50), [#53](https://github.com/Robomous/RoadMaker/issues/53) | Editor UX: read-only library panel + drag-to-place (#50); object/signal placement + properties (#72 — **includes the kernel edit commands** `AddObject`…`SetSignalValue` of `01` §2.4, with bindings, same PR); autosave & crash recovery (#53) | Author GS-1 props/signals in-editor; autosave restores after a simulated crash |
| 6 | [#51](https://github.com/Robomous/RoadMaker/issues/51), [#52](https://github.com/Robomous/RoadMaker/issues/52), [#73](https://github.com/Robomous/RoadMaker/issues/73) | Quality & release: esmini round-trip CI job (#51); minimal user guide (#52); GS-1 golden render + v0.5.0 checklist (#73) | GS-1 element checklist green; esmini loads exported `.xodr`; v0.5.0 tagged on green CI |

### Execution order & dependencies

Kernel phases are strictly ordered **0 → 1 → 2** (signals mirror the as-built
object patterns; object markings reuse the phase-0 `Object` model). Everything
else keys off these edges:

- **2b (#62)** touches only junctions — it can run any time after M2, in
  parallel with 0–3, but must land before 6.
- **3 (#70)** has no kernel dependency and can run in parallel with 0–2. It
  blocks **4** (textures/models) and **#50** (`assets/library.json` must exist).
- **4 (#71)** needs 2 (marking meshes + object dirty set) and 3 (textures,
  prop models).
- **5:** #50 needs 3; #72 needs 0, 1, and #50 (drag-to-place enters through the
  panel); #53 is independent (any time after M2).
- **6** is last; #73 depends on every other open M3a issue.

An executor picking "the next issue" takes the lowest unblocked phase; two
agents can safely work 2b or 3 in parallel with the kernel line.

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **`<objects>`/`<signals>` model breadth** — the object/signal chapters are large; over-modeling paints the data model into a corner, under-modeling drops foreign data | High | High | Scope the *authored* set to GS-1 (`01` §1); represent-and-round-trip (never drop) the rest via a raw-attribute passthrough; read the normative chapters first (done — `01` §7) |
| 2 | **Render-path growth** — instancing, textures, terrain, and sky in one milestone could destabilize the thin-renderer posture | Medium | High | Everything stays behind `Renderer` (`04` §1); sober mode remains the fallback and the packaging smoke path; each render feature lands behind its own toggle before becoming default |
| 3 | **Asset style incoherence** — mixed-style props read worse than none | Medium | Medium | One primary kit family for GS-1; style rule recorded in `03` §2 and [asset_candidates](../../roadmap/asset_candidates.md); poles are style-neutral, trees are not |
| 4 | **Manifest lock-in** — the runtime library-manifest format is committed in M3a but the M4 browser must subsume it | Medium | Medium | Design the manifest as a superset-friendly schema (`03` §4) with a version field; document the M4 extension points; no editing semantics baked in |
| 5 | **esmini license/distribution** drift blocks the CI gate | Low | Medium | Verify esmini's license + binary-distribution method at implementation time against the [dependency policy](../../standards/dependencies.md) (`05` §3); the gate is a smoke load, not a linked dependency |
| 6 | **Crosswalk/arrow as objects vs. signals ambiguity** — OpenDRIVE splits road markings between `<object>` markings and `<signal>` | Medium | Low | Follow the spec split (`01` §1, `02` §3): crosswalks/arrows/stop-lines are object markings; only traffic-control signs/lights are signals; each cites its chapter |

## Standards references — mandatory usage

All standards behavior in these docs cites the local ASAM texts under
`.claude/references/asam/` (OpenDRIVE 1.9.0 primary, 1.8.1 for deltas). Rule IDs
appear inline as `asam.net:xodr:<ver>:<rule>` and are emitted via the
`Diagnostic::rule_id` field. Primary chapters for M3a (contributors read the
local ASAM texts per the standing rule; see
[docs/domain/references.md](../../domain/references.md)):

- OpenDRIVE 1.9.0 **§13 Objects** — `<object>`, `<outline>`, `<repeat>`,
  `<skeleton>`.
- OpenDRIVE 1.9.0 **§14 Signals** — `<signal>`, orientation/`hOffset`,
  static vs. dynamic.
- OpenDRIVE 1.9.0 **§11.9 Lane road markings** — `<roadMark>` color and
  multi-line geometry.
- OpenDRIVE 1.9.0 **§12 Junctions** — junction `<boundary>` lane/joint
  segments and auxiliary boundary roads
  ([#62](https://github.com/Robomous/RoadMaker/issues/62), phase 2b).

Version handling: object/signal elements exist since ≤1.4; `<skeleton>` and
`@length` on signals are 1.8.0; `@temporary`/`@invalidated` and `<curveLocal>`
are 1.9.0. The writer targets the version already selected by the M2
version-explicit writer; 1.9.0-only attributes are emitted only when the target
is ≥1.9.0, with a code comment citing both chapters (`01` §6).
</content>
</invoke>
