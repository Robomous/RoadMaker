# Asset candidates for future milestones

*Scouted, license-compatible asset sources for M3a+ — candidates only.
Nothing on this page is fetched or committed until its milestone imports it
through the pipeline with per-file license verification.*

Policy context: the [asset standard](../../../standards/assets.md) — CC0 / MIT /
ISC / Apache-2.0 only, licenses verified **on the source page at fetch
time**, every file recorded in `ASSETS_LICENSES.md` via
`assets/manifest.json`. This page extends the scouting list started in the
[M2 assets design doc](../../../design/m2/05_assets.md) (which owns the M2
icon/texture inventory).

## 3D prop kits (M3a: vegetation, poles, signs)

| Source | Kits of interest | License | Notes |
|---|---|---|---|
| [Kenney](https://kenney.nl) | City Kit (Roads), City Kit (Suburban), Nature Kit | CC0 | Consistent low-poly style; roads/nature kits cover trees, shrubs, poles, benches. Record exact kit version + URL per file at fetch time |
| [Quaternius](https://quaternius.com) | Ultimate Nature, Road/Street packs | CC0 | Complementary style — pick ONE primary style per scene to avoid a mismatched look; decide Kenney vs Quaternius as primary during M3a asset selection |

Style guidance for M3a: choose one kit family as the primary vegetation/prop
style for GS-1; mixing families is allowed only where silhouettes are
compatible (e.g. poles are style-neutral, trees are not).

## Vehicles (M4: actors, incl. the GS-3 ambulance)

| Source | Assets of interest | License | Notes |
|---|---|---|---|
| [Kenney](https://kenney.nl) | Car Kit | CC0 | Includes emergency vehicles in some kit versions — verify the ambulance model exists at fetch time |
| [Quaternius](https://quaternius.com) | Ultimate Vehicles / Cars | CC0 | Alternative style; same one-primary-style rule |

## Terrain / surface textures (M3a: extends the M2 set)

| Source | Materials | License | Notes |
|---|---|---|---|
| [ambientCG](https://ambientcg.com) | Asphalt, concrete, grass/ground | CC0 | Primary source (already the M2 choice); 1K downloads, re-encoded per the pipeline rules |
| [Poly Haven](https://polyhaven.com) | Same categories + HDRIs | CC0 | Fallback; HDRIs are candidates for the M3a sky/lighting pass |

Grass/ground remains procedural-first per the M2 decision; textures are for
where procedural reads poorly (close-ups, transitions).

## Traffic-sign faces (M3a)

- **US MUTCD sign graphics** — US-federal public domain; suitable
  per-file after verification.
- **Vienna-convention sign SVGs on Wikimedia Commons** — licensing is
  **per file** (PD-self, CC0, CC-BY mixtures). Mandate: each sign file is
  verified and recorded individually at fetch time; CC-BY files are skipped
  or escalated for maintainer approval. Never bulk-import a category.

## Explicitly out of bounds

- Any 3D asset, texture, or sign graphic from a commercial road-editor
  product or its samples — see the
  [product-parity rules](../../../standards/product-parity.md).
- CC-BY-NC / CC-BY-ND anything; unlicensed model-sharing-site downloads.

## Process reminders

- Candidates enter the repo only through `assets/manifest.json` +
  `scripts/fetch_assets.py`, with the `ASSETS_LICENSES.md` row in the same
  commit; `scripts/check_asset_licenses.py` gates CI.
- OSM **data** for GS-2 is not an asset in this sense but carries its own
  ODbL attribution duty — recorded with the extract, see the
  [GS-2 spec](golden_scenes/gs2_imported_district.md).
