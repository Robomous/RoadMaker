# Asset licenses

Ledger for every binary/media asset in the repository (icons, textures,
models, fonts, images). Policy — see [`docs/standards/assets.md`](docs/standards/assets.md):

- Allowed licenses: **CC0, MIT, ISC, Apache-2.0** (public-domain US-federal
  works count as CC0 here).
- CC-BY only with explicit maintainer approval; CC-BY-NC/ND and unlicensed
  material never.
- Licenses are verified on the source page at retrieval time, not assumed.
- Every file under `assets/` and `editor/resources/` must have a row in this
  table (CI enforces via `scripts/check_asset_licenses.py`). Fetched assets
  additionally carry a manifest entry in `assets/manifest.json`.
- **AI-generated** assets (permitted for textures and simple original graphics,
  per [`docs/standards/assets.md`](docs/standards/assets.md#ai-generated-assets))
  use `License = MIT` with the **Source** column set to
  *"AI-generated original work (tool, date)"* and **Author** the person who
  generated it.

| File | Source | Author | License | Retrieved |
|---|---|---|---|---|
| `assets/textures/asphalt/asphalt_02_diff_512.jpg` | [Poly Haven — asphalt_02](https://polyhaven.com/a/asphalt_02) (resized to 512, re-encoded JPEG q85) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-14 |
| `assets/textures/concrete/brushed_concrete_diff_512.jpg` | [Poly Haven — brushed_concrete](https://polyhaven.com/a/brushed_concrete) (resized to 512, re-encoded JPEG q85) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-14 |
| `editor/resources/icons/custom/clothoid-road.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/junction-connect.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/lane-section.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/template-highway.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/template-rural.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/template-urban.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/lucide/box.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/circle-plus.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/git-merge.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-12 |
| `editor/resources/icons/lucide/file-output.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/file-plus.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/folder-open.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/info.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/magnet.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/mountain.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/mouse-pointer-2.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/move.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/octagon-x.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/redo-2.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/rotate-ccw.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/save.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/scan.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/scissors.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-12 |
| `editor/resources/icons/lucide/trash-2.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/trees.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-13 |
| `editor/resources/icons/lucide/triangle-alert.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/undo-2.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `editor/resources/icons/lucide/waypoints.svg` | [lucide 1.24.0](https://github.com/lucide-icons/lucide/tree/1.24.0) | Lucide Contributors | ISC | 2026-07-10 |
| `assets/logo/robomous-original.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/README.md` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/roadmaker.icns` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/roadmaker.ico` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/roadmaker.rc` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/roadmaker.desktop` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_16.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_24.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_32.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_48.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_64.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_128.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_256.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `editor/resources/branding/icon_512.png` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_pine.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_pine.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_oak.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_oak.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_birch.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_birch.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_poplar.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/tree_poplar.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/shrub.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/shrub.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-13 |
| `assets/library/props/signal_light.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-14 |
| `assets/library/props/signal_light.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-14 |
| `assets/library/props/sign_generic.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-14 |
| `assets/library/props/sign_generic.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-14 |
| `docs/standards/golden-look.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/create-road.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/junction.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/elevation.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/library.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/workflow.gif` | original work (this repository), editor screen recording | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/gs1_hero.png` | original work (this repository), editor screenshot rendered by CI (`visual-artifacts`) | RoadMaker contributors | MIT | 2026-07-15 |
| `docs/roadmap/golden_scenes/img/gs1_baseline_v0.6.0.png` | original work (this repository), viewport render from the GS-1 fixed camera, rendered by CI (`visual-artifacts`) | RoadMaker contributors | MIT | 2026-07-15 |
