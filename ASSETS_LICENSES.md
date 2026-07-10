# Asset licenses

Ledger for every binary/media asset in the repository (icons, textures,
models, fonts, images). Policy — see `docs/m2/05_assets.md`:

- Allowed licenses: **CC0, MIT, ISC, Apache-2.0** (public-domain US-federal
  works count as CC0 here).
- CC-BY only with explicit maintainer approval; CC-BY-NC/ND and unlicensed
  material never.
- Licenses are verified on the source page at retrieval time, not assumed.
- Every file under `assets/` and `editor/resources/` must have a row in this
  table (CI enforces via `scripts/check_asset_licenses.py`). Fetched assets
  additionally carry a manifest entry in `assets/manifest.json`.

| File | Source | Author | License | Retrieved |
|---|---|---|---|---|
| `editor/resources/icons/custom/clothoid-road.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
