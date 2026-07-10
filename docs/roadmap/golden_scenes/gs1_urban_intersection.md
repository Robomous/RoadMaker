# GS-1 — Urban intersection

*Golden scene for M3a (visual & standards completeness): a signalized 4-arm
urban junction that exercises every new kernel feature, asset class, and
render capability of the milestone.*

Specified purely from ASAM OpenDRIVE concepts, per the
[product-parity rules](../../standards/product-parity.md).

## Scene definition

A flat 4-arm junction of two urban two-way streets crossing at 90°:

- **Arms:** four roads, each ~80 m long, meeting in one junction. Each road:
  two driving lanes per direction (`driving`), a parking-free curb lane
  omitted, one `sidewalk` lane per side behind a `curb`-height offset.
  Reference lines are straight (`line` records); the junction's connecting
  roads use arcs/spirals as produced by the junction tool.
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

Each row cites the kernel feature and asset it depends on — rows land as
their features land, and the scene is done when every row is checked
against a current render from the fixed camera.

| # | Element | Kernel feature | Asset | Render |
|---|---|---|---|---|
| 1 | ☐ Four-arm junction, valid lane links | M2 junction tools (exists after M2) | — | junction surface (M2) |
| 2 | ☐ Textured asphalt roadway | per-lane material model (exists) | ambientCG asphalt (CC0) | textured mode default ON |
| 3 | ☐ Concrete sidewalks with curb offset | `sidewalk` lane type (exists), curb elevation | ambientCG concrete (CC0) | textured mode |
| 4 | ☐ Crosswalks on all four arms | **new:** `<object type="crosswalk">` parse/write/validate | marking-paint material | object outline → mesh |
| 5 | ☐ Lane arrows (left/straight/right) | **new:** arrow road-mark objects | arrow glyph geometry (generated) | road-mark mesh |
| 6 | ☐ Stop lines at each approach | **new:** stop-line road mark | — | road-mark mesh |
| 7 | ☐ Center double-yellow lines | **new:** multi-line road marks (`solid solid`, color) | — | dual-strip marking mesh |
| 8 | ☐ Dashed white lane lines | broken road marks (exists) | — | existing marking path |
| 9 | ☐ Traffic lights ×4 | **new:** `<signal>` dynamic parse/write/validate | signal-head + pole model (CC0) | prop instancing |
| 10 | ☐ Speed-limit + crossing signs | **new:** `<signal>` static entries | verified public-domain sign faces + pole (CC0) | prop instancing |
| 11 | ☐ ~20 vegetation props | **new:** `<object>` prop instances | vegetation kit (CC0, see [asset candidates](../asset_candidates.md)) | prop instancing |
| 12 | ☐ Grass terrain around roads | **new:** terrain skirt + procedural ground | procedural (no fetch) | ground shader |
| 13 | ☐ Sky and daytime lighting | — | procedural sky or CC0 HDRI | sky/lighting pass |
| 14 | ☐ Sober mode still available | — | — | render-mode toggle |

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

## History

- 2026-07-10 — initial spec (this document). Camera not yet exercised; the
  first render lands with M3a's release PR.
