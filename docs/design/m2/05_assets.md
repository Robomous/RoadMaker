# M2 assets — inventory, sources, licenses, pipeline

Policy (mirrors the code dependency policy): **CC0, MIT, ISC, Apache-2.0 only.**
No CC-BY without explicit maintainer approval (attribution burden in
installers), never CC-BY-NC/ND, never unlicensed images. Every asset gets a row
in `ASSETS_LICENSES.md`: file, source URL, author, license, retrieval date.
**Licenses are verified on the source page at fetch time** — this document's
mappings are candidates, not facts, until the manifest row records the
verification.

## 1. Toolbar / UI icons

- **Primary set: Lucide** (lucide.dev, ISC) — stroke-consistent 24×24, 2 px,
  round caps. **Fallbacks:** Tabler Icons (MIT), Material Symbols (Apache-2.0)
  for gaps. **Excluded:** Breeze/Oxygen (LGPL assets), Font Awesome free
  (CC-BY).
- Style rule: monochrome line icons only, recolored to the theme palette at
  load; no colored icon soup. (The original "default Qt widget chrome" rule
  was superseded 2026-07-12 by the themed-editor decision —
  see `docs/standards/ui-design.md`.)

### Icon mapping (candidate Lucide names — verify at fetch time)

| Action | Icon | Set |
|---|---|---|
| Open | `folder-open` | Lucide |
| Save | `save` | Lucide |
| Undo / Redo | `undo-2` / `redo-2` | Lucide |
| Select (cursor) | `mouse-pointer-2` | Lucide |
| Move | `move` | Lucide |
| Add node | `circle-plus` | Lucide |
| Edit nodes | `waypoints` | Lucide |
| Create road | custom `clothoid-road` | custom |
| Lane profile | custom `lane-section` | custom |
| Create junction | custom `junction-connect` | custom |
| Elevation | `mountain` | Lucide |
| Delete | `trash-2` | Lucide |
| Snap toggle | `magnet` | Lucide |
| Camera reset | `rotate-ccw` | Lucide |
| Frame selection | `scan` (or `frame`) | Lucide |
| Export glTF | `box` | Lucide |
| Export USD | `file-output` | Lucide |
| Diagnostics: error | `octagon-x` | Lucide |
| Diagnostics: warning | `triangle-alert` | Lucide |
| Diagnostics: info | `info` | Lucide |
| Road templates (rural/urban/highway) | custom `template-*` variants | custom |

### Custom domain icons

No general set has clothoid-road / lane-section / junction-with-connecting-roads
/ road-template glyphs. Drawn as SVGs **on Lucide's grid** (24×24 viewBox,
2 px stroke, `stroke="currentColor"`, round caps/joins, no fills) so they blend
in. Location: `editor/resources/icons/custom/`; licensed MIT as project assets
(rows in `ASSETS_LICENSES.md` with author = RoadMaker contributors).

### Integration

- SVGs under `editor/resources/icons/{lucide,tabler,material,custom}/`, compiled
  via `editor/resources/resources.qrc` into the editor binary.
- One helper: `Icons::get("road")` (`editor/src/app/icons.{hpp,cpp}`).
  **Recoloring decision: QSvgRenderer → QPixmap tinting.** The helper loads the
  SVG (all icons use `currentColor`, which QSvgRenderer leaves black), renders
  to a pixmap at the required device pixel ratios, and tints via
  `QPainter::CompositionMode_SourceIn` with the palette's `WindowText` color —
  yielding dark/light correctness with zero per-icon stylesheet work. Disabled
  state: same pipeline with the `Disabled` palette color, registered on the
  `QIcon` (`QIcon::addPixmap(…, QIcon::Disabled)`). Palette-change
  (`QEvent::ApplicationPaletteChange`) invalidates the helper's cache.
- Requires the **QtSvg module**: add `qtsvg` to `scripts/setup_qt.py` archives,
  `Svg` to the editor `find_package(Qt6 …)`, and the deploy expectations
  (macdeployqt/windeployqt handle it automatically once linked; AppImage's
  linuxdeploy-plugin-qt picks it up from the link dependency — verify in the
  packaging smoke test). This is the single sanctioned Qt module addition of M2.

## 2. Viewport textures (optional textured mode)

Per-lane flat materials remain the default. An optional textured mode adds
asphalt / concrete-sidewalk / grass / marking-paint materials.

**Procedural vs texture, per material (the M2 call):**

| Material | Approach | Rationale |
|---|---|---|
| Lane markings | **Procedural** (shader stripe from the existing marking submeshes' UV-free geometry — markings are separate geometry already; paint gets a slight noise term) | Crisp at all zooms; textures alias badly on thin strips |
| Asphalt | **Texture** (ambientCG `Asphalt025` or similar, 1K) | Convincing asphalt noise is cheaper to fetch than to author in-shader; low-frequency detail matters in close-ups |
| Concrete / sidewalk | **Texture** (ambientCG `Concrete0xx`, 1K) | same |
| Grass / ground | **Procedural** (two-tone value noise) | Ground plane is context, not content; avoids tiling artifacts on large areas |

Sources: **ambientCG** (ambientcg.com, CC0) primary, **Poly Haven**
(polyhaven.com, CC0) fallback. Download 1K only; re-encode to **KTX2 (BasisU,
UASTC for normal maps if ever needed; ETC1S for albedo) or PNG, ≤ 512 KB per
file**; commit under `assets/textures/` with manifest rows recording the
*original* URL + the processing command line. Renderer support (a UV channel on
lane patches + a sampler path in `GLRenderer`) is part of the Phase 2+ textured
mode task, OFF by default.

## 3. Future prop/sign assets (M3 scouting — nothing ships in M2)

- 3D props (signs, poles, cones, trees): candidate kits — **Kenney.nl**
  "Road Kit" / "Nature Kit" (CC0), **Quaternius** "Ultimate Road/Cars" packs
  (CC0). Record exact kit versions + URLs in the manifest when M3 imports them.
- Traffic-sign faces: US MUTCD sign graphics are US-federal public domain;
  Vienna-convention SVGs on Wikimedia Commons are **per-file** licensed (PD-self,
  CC0, CC-BY mixtures). Mandate: each sign file is verified and recorded
  individually at fetch time; CC-BY files are skipped (or escalated for
  approval). No sign assets in M2.

## 4. Pipeline

- `assets/manifest.json` — checked in; one entry per fetched asset:
  `{ "url", "sha256", "license", "license_url", "author", "destination",
  "retrieved", "postprocess" }`.
- `scripts/fetch_assets.py` — reads the manifest, downloads, verifies sha256,
  applies recorded postprocess steps (resize/re-encode), places files. Icons and
  processed textures ARE committed (small, license-clean); the script exists for
  regeneration, upgrades, and future too-big-to-commit assets. Stdlib-only
  (urllib + hashlib) so it runs anywhere CI does.
- `scripts/check_asset_licenses.py` — CI lint step: every file under `assets/`
  and `editor/resources/` (excluding `.qrc`, `README`) has a row in
  `ASSETS_LICENSES.md`; every manifest entry's license ∈ allowed set; fails the
  lint job otherwise.
- `ASSETS_LICENSES.md` — human-readable ledger (the CI check keeps it honest).
