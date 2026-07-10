# Asset policy

*License rules and pipeline for every binary/media asset in the repository — icons, textures, models, fonts, images. Code dependencies have their own page: [dependencies](./dependencies.md).*

## License policy

- Allowed licenses: **CC0, MIT, ISC, Apache-2.0** only (US-federal
  public-domain works count as CC0 here).
- **CC-BY** requires explicit maintainer approval — attribution is a real
  burden in installers and About dialogs, so it is the exception, not the
  rule.
- **Never** CC-BY-NC, CC-BY-ND, or unlicensed material.
- **Licenses are verified on the source page at fetch time.** Any icon/texture
  mapping in a design doc is a *candidate*, not a fact, until the manifest row
  records the verification (source URL, author, license, retrieval date).
- Every asset gets a row in `ASSETS_LICENSES.md` at the repo root: file,
  source URL, author, license, retrieval date. CI enforces this (see the
  pipeline section below).

## Approved sources

**UI icons**

- Primary set: **Lucide** (lucide.dev, ISC).
- Fallbacks for gaps: **Tabler Icons** (MIT), **Material Symbols**
  (Apache-2.0).
- Excluded: Breeze/Oxygen (LGPL assets), Font Awesome Free (icons are
  CC-BY).

**Viewport textures**

- Primary: **ambientCG** (ambientcg.com, CC0).
- Fallback: **Poly Haven** (polyhaven.com, CC0).

**Future 3D props and signs** (scouted, nothing shipped yet)

- Candidate kits: **Kenney.nl** and **Quaternius** packs (CC0). Sign
  graphics need per-file verification — Wikimedia Commons SVGs are licensed
  individually. The scouted list lives in
  [roadmap/asset_candidates](../roadmap/asset_candidates.md).

## Icon style rules

- Monochrome line icons only, recolored to the palette at load time — no
  colored icon soup.
- All icons follow the Lucide grid: 24×24 viewBox, 2 px stroke,
  `stroke="currentColor"`, round caps/joins, no fills. Fallback-set icons are
  chosen to blend with this style.
- Custom domain glyphs (clothoid road, lane section, junction, road
  templates — shapes no general icon set has) are drawn as SVGs on the same
  grid, live under `editor/resources/icons/custom/`, and are licensed MIT as
  project assets (their `ASSETS_LICENSES.md` rows list author = RoadMaker
  contributors).

## Pipeline

Four pieces keep the asset inventory honest:

1. **`assets/manifest.json`** — checked in; one entry per fetched asset:

   ```json
   {
     "url": "...",
     "sha256": "...",
     "license": "...",
     "license_url": "...",
     "author": "...",
     "destination": "...",
     "retrieved": "YYYY-MM-DD",
     "postprocess": "..."
   }
   ```

2. **`scripts/fetch_assets.py`** — reads the manifest, downloads, verifies
   the SHA256, applies the recorded post-processing steps (resize/re-encode),
   and places files. It is stdlib-only (urllib + hashlib) so it runs anywhere
   CI does. Icons and processed textures *are* committed (small,
   license-clean); the script exists for regeneration, upgrades, and future
   assets too large to commit.

   ```sh
   python3 scripts/fetch_assets.py
   ```

3. **`scripts/check_asset_licenses.py`** — the CI lint step: every file under
   `assets/` and `editor/resources/` (excluding `.qrc` and README files) must
   have a row in `ASSETS_LICENSES.md`, and every manifest entry's license
   must be in the allowed set. The lint job fails otherwise.

   ```sh
   python3 scripts/check_asset_licenses.py
   ```

4. **`ASSETS_LICENSES.md`** — the human-readable ledger at the repo root; the
   CI check keeps it honest.

For the M2-specific asset inventory (icon mappings, texture choices,
procedural-vs-texture decisions), see the
[M2 assets design doc](../design/m2/05_assets.md).
