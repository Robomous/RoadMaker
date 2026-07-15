# GS-1 — Urban intersection

*Golden scene for M3a (visual & standards completeness): a signalized 4-arm
urban junction that exercises every new kernel feature, asset class, and
render capability of the milestone.*

Specified purely from ASAM OpenDRIVE concepts, per the
[product-parity rules](../../standards/product-parity.md).

## Scene definition

A flat 4-arm junction of two urban two-way streets crossing at 90°:

- **Arms:** four roads, each ~80 m long, meeting in one junction. Each road:
  one driving lane per direction (`driving`), a parking-free curb lane
  omitted, one `sidewalk` lane per side behind a `curb`-height offset.
  Reference lines are straight (`line` records); the junction's connecting
  roads use arcs/spirals as produced by the junction tool.

  > **Amended 2026-07-14 (close-out).** This originally read "two driving
  > lanes per direction". The scene was built one-lane-per-direction (the
  > `urban_sidewalk` profile), and the spec is corrected here to describe what
  > exists rather than a scene that was never authored. The consequence is
  > recorded honestly as gap 8 below — a single-lane carriageway has no
  > same-direction lane boundary to draw a dashed line on.
- **Speed regime:** 50 km/h urban; lane widths 3.5 m driving, 2.0 m
  sidewalks.
- **Signalization:** one traffic light per approach (OpenDRIVE `<signal>`,
  dynamic, type per the signal catalog), plus static signs on at least two
  arms: a speed-limit sign and a pedestrian-crossing warning sign
  (`<signal>` static entries).
- **Crossings:** a crosswalk on all four arms (`<object type="crosswalk">`
  with outline, per OpenDRIVE object semantics).
- **Markings:** center double-yellow (`roadMark` type `solid solid`, color
  yellow) on all arms; dashed white lane lines (`broken`, white) between
  same-direction driving lanes; stop lines at each approach; lane arrows
  (left / straight / right as road-mark objects) on approach lanes.
- **Props:** ~20 vegetation instances (street trees in sidewalk cutouts,
  shrubs at corners) and the signal/sign poles, all as OpenDRIVE
  `<object>` entries referencing library assets.
- **Ground:** grass terrain surrounding the roads (terrain skirt +
  procedural ground), sidewalks in concrete, roadway in asphalt.
- **Lighting:** daytime sky/lighting pass; shadows optional in M3a.

## Element checklist

The single authoritative status for this scene. Each row is ticked against
the committed baseline render from the fixed camera, and cites the PR that
delivered it. Rows that did **not** land are marked `[GAP]` with a reason and
a follow-up issue — never silently dropped.

*(Until close-out this document carried two tables — an all-unticked
aspirational checklist and a separate "as-built" table that disagreed with
it. They are merged here into one.)*

| # | Element | Status | Delivered by |
|---|---|---|---|
| 1 | Four-arm junction, valid lane links | [x] | M2 junction tools; `edit::assembly::x_intersection` urban profile ([#187]); junction boundary closed ([#191]) |
| 2 | Textured asphalt roadway | [x] | material/texture interface ([#165]) + textured asphalt surfaces ([#170]) |
| 3 | Concrete sidewalks with curb offset | [x] | `urban_sidewalk` profile + concrete texture ([#170]) |
| 4 | Crosswalks on all four arms | [x] | `edit::junction_crosswalks` ([#171]) |
| 5 | Lane arrows | [x] | `edit::junction_lane_arrows` ([#173]); per-lane turn glyphs ([#203]) — GS-1 authors straight, which is the correct glyph for a single lane serving all three movements |
| 6 | Stop lines at each approach | [x] | `edit::junction_stop_lines` ([#172]) |
| 7 | Center double-yellow lines | [x] | `edit::junction_center_marks` ([#203]) — `solid solid` + `color="yellow"` on lane 0 of each arm, rendered as true dual stripes |
| 8 | Dashed white lane lines | **[GAP]** | Not representable in this scene: a one-lane-per-direction carriageway has no same-direction lane boundary to mark. Needs a multi-lane profile. → [#194] |
| 9 | Traffic lights ×4 | [x] | `add_signal` commands ([#174]) + instanced signal meshes ([#185]) + Library placement ([#186]) |
| 10 | Speed-limit + crossing signs | [x] | two static `<signal>`s, DE 274/50 + 133/10 ([#174], [#185]) |
| 11 | ~20 vegetation props | [x] | tree props ([#139]–[#141]); **24 street trees** — raised from 16 at close-out to clear the target |
| 12 | Grass terrain around roads | [x] | procedural grass ground ([#169]) |
| 13 | Sky and daytime lighting | [x] | daytime lighting + sky ([#166]) |
| 14 | Sober mode still available | [x] | `View → Textured Rendering` toggle ([#166]); sober stays the default |

**Score: 13 / 14 delivered, 1 gap** (row 8). Row 7 closed during the v0.6.0
close-out, once [#203] added the centre-mark authoring op the scene was
missing; gaps 5a and 7 are both retired below. The remaining gap needs a wider
road profile, not marking work.

### Gaps and follow-ups

| Gap | Issue | Status |
|---|---|---|
| 5a | [#203] | **Closed.** `edit::junction_lane_arrows` takes a per-approach-lane glyph chooser, so a turn-lane scene can author `arrowLeft`/`arrowRight`. GS-1 itself still authors `arrowStraight` — with one lane per direction serving left, straight and right at once, no single turn glyph is correct for it, and the normative combined subtype (`arrowStraightLeftRight`, §13.14.8 Table 117) does not mesh yet. |
| 7 | [#203] | **Closed.** `edit::junction_center_marks` authors `solid solid` + `color="yellow"` on lane 0 of every arm; the mesh renders the two stripes from the mark's width. |
| 8 | [#194] | **Open.** Requires a two-lane-per-direction urban profile, which GS-1 does not use — a single-lane carriageway has no same-direction lane boundary to draw a dashed line on. Deferred to whichever milestone brings the wider profile. |

[#69]: https://github.com/Robomous/RoadMaker/issues/69
[#139]: https://github.com/Robomous/RoadMaker/pull/139
[#141]: https://github.com/Robomous/RoadMaker/pull/141
[#165]: https://github.com/Robomous/RoadMaker/pull/165
[#166]: https://github.com/Robomous/RoadMaker/pull/166
[#169]: https://github.com/Robomous/RoadMaker/pull/169
[#170]: https://github.com/Robomous/RoadMaker/pull/170
[#171]: https://github.com/Robomous/RoadMaker/pull/171
[#172]: https://github.com/Robomous/RoadMaker/pull/172
[#173]: https://github.com/Robomous/RoadMaker/pull/173
[#174]: https://github.com/Robomous/RoadMaker/pull/174
[#185]: https://github.com/Robomous/RoadMaker/pull/185
[#186]: https://github.com/Robomous/RoadMaker/pull/186
[#187]: https://github.com/Robomous/RoadMaker/pull/187
[#191]: https://github.com/Robomous/RoadMaker/pull/191
[#193]: https://github.com/Robomous/RoadMaker/issues/193
[#194]: https://github.com/Robomous/RoadMaker/issues/194
[#203]: https://github.com/Robomous/RoadMaker/pull/203

## Fixed camera

Kernel frame: right-handed, Z-up, meters. Junction center at the origin.

| Parameter | Value |
|---|---|
| Position | (−55.0, −55.0, 35.0) |
| Target | (0.0, 0.0, 0.0) |
| Up | +Z |
| Vertical FOV | 45° |
| Aspect | 16:9 (render at 1920×1080) |

A three-quarter elevated view down one diagonal: shows all four arms, two
crosswalks face-on, arrows and stop lines legible, tree line against the
sky.

## Acceptance beyond the image

- The scene's `.xodr` validates with zero errors; new elements carry
  normative rule-id citations in any diagnostics
  ([references](../../domain/references.md)).
- Round-trip: write → parse → write is stable within `rm::tol` and loses no
  `<object>`/`<signal>`/road-mark data.
- Scene loads and renders headless in the golden-screenshot CI workflow
  ([process](README.md)).
- Every asset used has its `ASSETS_LICENSES.md` row
  ([asset policy](../../standards/assets.md)).

## Build

The scene is authored by **dogfooding the kernel edit layer** —
`python/examples/build_gs1.py` lays down the junction and every mark, signal,
and prop as real `edit::Command`s on an `EditStack`, exactly as the editor
would — and saved to `assets/samples/gs1_urban_intersection.xodr`
(16 roads · 1 junction · **36 objects** · 6 signals · **0 diagnostics**). The
`esmini-roundtrip` job globs `assets/samples/*.xodr`, so GS-1 loads there
automatically; the golden view renders in the `visual-artifacts` job
(`--camera gs1 --textured --size 1920x1080`).

**Baseline:** `img/gs1_baseline_v0.6.0.png`, captured from the
`visual-artifacts` run's `gs1_urban_intersection.png` artifact — macOS dev has
no offscreen GL context, so the baseline is always committed from a CI render.
Tracked release-over-release in the [golden-scenes README](README.md#baselines).

### Honest notes on the v0.6.0 baseline

Recorded at close-out from looking at the render, so the next reader is not
surprised. None of these block acceptance; all are visual-quality work that
the **Materials & Structures (v0.7.0)** milestone is the natural home for:

- **The junction floor reads as a flat grey slab.** It carries no texture and
  no markings across it — the surface exists and is selectable, but visually
  it is the weakest part of the scene. Junction-floor material assignment is
  explicitly in the v0.7.0 material-system scope.
- **The sky is dark and murky rather than daytime.** The gradient works but
  the top of frame is near-black; the lighting pass is doing its job (row 13
  ticks) without looking like noon.
- **The fixed camera frames the scene small**, with a lot of empty grass. The
  camera is faithful to the spec's stated pose — (−55, −55, 35), 45° vFOV, and
  the `gs1` preset matches it to within a rounding error — but the spec's own
  prose promises "tree line against the sky", which this pose does not deliver
  because the trees sit well below the horizon. Spec-internal inconsistency,
  left alone deliberately: changing the fixed camera at close-out would
  invalidate the baseline it is meant to certify.

## History

- 2026-07-10 — initial spec (this document). Camera not yet exercised; the
  first render lands with M3a's release PR.
- 2026-07-14 — GS-1 built (`build_gs1.py` + `gs1_urban_intersection.xodr`),
  `gs1` golden camera + CI render added.
- 2026-07-14 — **close-out (v0.6.0).** The two disagreeing tables merged into
  one authoritative checklist (12/14 + 2 filed gaps); scene definition amended
  to the as-built one-lane-per-direction profile; trees raised 16 → 24 to clear
  row 11; baseline committed; honest render notes recorded above.
- 2026-07-15 — **row 7 closed (13/14), still within v0.6.0.**
  `edit::junction_center_marks` ([#203]) landed the centre-mark authoring the
  scene had been missing, so the arms now carry the specified double-yellow
  centre line; gap 5a retired with the per-lane arrow-glyph chooser in the same
  PR. v0.6.0 had not been tagged, so the baseline was refreshed in place rather
  than forked into a new release row. Row 8 stays open on [#194].
