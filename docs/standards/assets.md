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

**Sign artwork** (shipped)

- The US sign pack's symbols are **authored in-repo**, not sourced: they live
  in `assets/signs/us/` and are drawn after the **public-domain US federal sign
  specifications**, with no third-party artwork file copied. That side-steps the
  per-file verification third-party sign graphics would need (Wikimedia Commons
  SVGs, for instance, are licensed individually) and keeps the whole pack
  Apache-2.0 project work.

**Future 3D props** (scouted, nothing shipped yet)

- Candidate kits: **Kenney.nl** and **Quaternius** packs (CC0). The scouted
  list lives in
  [roadmap/asset_candidates](../roadmap/archive/2026-07-pre-reset/asset_candidates.md).

## AI-generated assets

AI-generated assets are **permitted for textures and simple original graphics**
under the rules below (maintainer decision, 2026-07-13). They may be revisited if
provenance norms shift.

- **Provenance recorded per asset:** the generating tool + version, the date, and
  the author (the person who generated it) are recorded — in the commit and in
  the asset's `ASSETS_LICENSES.md` row.
- **Prompt hygiene:** prompts must **not** request the style of identifiable
  artists, products, or brands, nor reproduce logos or trade dress. Output is
  reviewed against the [product-parity IP rules](product-parity.md) before commit.
- **Licensing:** AI-generated original work is licensed **Apache-2.0** as project assets;
  its `ASSETS_LICENSES.md` row reads *"AI-generated original work (tool, date)"*
  in the source/author columns.
- **Preference order (unchanged):** (1) **CC0 libraries**, (2) **procedural**,
  (3) **AI-generated** for missing variants, (4) **commissioned**. AI generation
  fills gaps a CC0 library or a procedural approach doesn't cover — it is not the
  first reach.

## Icon style rules

- Monochrome line icons only, recolored to the palette at load time — no
  colored icon soup.
- All icons follow the Lucide grid: 24×24 viewBox, 2 px stroke,
  `stroke="currentColor"`, round caps/joins, no fills. Fallback-set icons are
  chosen to blend with this style.
- Custom domain glyphs (clothoid road, lane section, junction, road
  templates — shapes no general icon set has) are drawn as SVGs on the same
  grid, live under `editor/resources/icons/custom/`, and are licensed Apache-2.0 as
  project assets (their `ASSETS_LICENSES.md` rows list author = RoadMaker
  contributors).

## SVG artwork (not icons)

Sign symbols are **artwork, not icons**, and the icon rules above do not apply
to them: they carry fills and full colour, and their viewBox is the sign face's
own proportions rather than a 24×24 grid. They live in `assets/signs/<country>/`,
one file per designation, licensed Apache-2.0 as project work with author =
RoadMaker contributors.

Two constraints come from the consumer rather than from style. The kernel
rasterises them (`roadmaker::signs::render_face`) because faces are baked
headless by the glTF exporter and from Python, and `core/` must never link Qt —
so the rasteriser is **nanosvg**, which parses shapes only. No `<text>`: every
legend is a text layer the renderer draws, which is also what keeps legends
editable and consistently typeset. `scripts/gen_sign_symbols.py` embeds the SVG
source text into the kernel, so there is no runtime file IO and no rasteriser at
build time.

## Fonts

Fonts are assets, and the allowed-license set has one **maintainer-approved
exception** for them: **SIL OFL-1.1**. It exists because the open faces that
match real-world signage are OFL, and the OFL's obligations are satisfied by
shipping the licence text beside the font, which
`assets/fonts/Overpass-LICENSE.md` does. When a font is offered under more than
one licence, the `ASSETS_LICENSES.md` row must record **which one RoadMaker
takes** — Overpass is dual OFL-1.1/LGPL-2.1 and we take the OFL half, so Qt
remains the project's only LGPL dependency.

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
