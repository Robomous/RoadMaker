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
| `assets/textures/asphalt/asphalt_02_nor_gl_512.png` | [Poly Haven — asphalt_02](https://polyhaven.com/a/asphalt_02) (resized to 512, PNG — normals kept lossless) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-17 |
| `assets/textures/asphalt/asphalt_02_rough_512.jpg` | [Poly Haven — asphalt_02](https://polyhaven.com/a/asphalt_02) (resized to 512, re-encoded JPEG q85) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-17 |
| `assets/textures/asphalt/worn_asphalt_diff_512.jpg` | [Poly Haven — worn_asphalt](https://polyhaven.com/a/worn_asphalt) (resized to 512, re-encoded JPEG q85) | Poly Haven (Amal Kumar) | CC0 | 2026-07-17 |
| `assets/textures/asphalt/worn_asphalt_nor_gl_512.png` | [Poly Haven — worn_asphalt](https://polyhaven.com/a/worn_asphalt) (resized to 512, PNG — normals kept lossless) | Poly Haven (Amal Kumar) | CC0 | 2026-07-17 |
| `assets/textures/asphalt/worn_asphalt_rough_512.jpg` | [Poly Haven — worn_asphalt](https://polyhaven.com/a/worn_asphalt) (resized to 512, re-encoded JPEG q85) | Poly Haven (Amal Kumar) | CC0 | 2026-07-17 |
| `assets/textures/concrete/brushed_concrete_diff_512.jpg` | [Poly Haven — brushed_concrete](https://polyhaven.com/a/brushed_concrete) (resized to 512, re-encoded JPEG q85) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-14 |
| `assets/textures/concrete/brushed_concrete_nor_gl_512.png` | [Poly Haven — brushed_concrete](https://polyhaven.com/a/brushed_concrete) (resized to 512, PNG — normals kept lossless) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-17 |
| `assets/textures/concrete/brushed_concrete_rough_512.jpg` | [Poly Haven — brushed_concrete](https://polyhaven.com/a/brushed_concrete) (resized to 512, re-encoded JPEG q85) | Poly Haven (Rob Tuytel, Sergej Majboroda) | CC0 | 2026-07-17 |
| `editor/resources/icons/custom/chevrons-right.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-20 |
| `editor/resources/icons/custom/clothoid-road.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/corner-radius.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-20 |
| `editor/resources/icons/custom/stop-line.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-20 |
| `editor/resources/icons/custom/crosswalk.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-18 |
| `editor/resources/icons/custom/junction-span.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-20 |
| `editor/resources/icons/custom/junction-surface.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-21 |
| `editor/resources/icons/custom/tool_maneuver.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-21 |
| `editor/resources/icons/custom/tool_signal.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-21 |
| `editor/resources/icons/custom/junction-connect.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/lane-section.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/marking-point.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-18 |
| `editor/resources/icons/custom/marking-curve.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-18 |
| `editor/resources/icons/custom/prop-curve.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-19 |
| `editor/resources/icons/custom/prop-point.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-19 |
| `editor/resources/icons/custom/prop-polygon.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-19 |
| `editor/resources/icons/custom/prop-span.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-19 |
| `editor/resources/icons/custom/template-highway.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/template-rural.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/icons/custom/template-urban.svg` | original work (this repository) | RoadMaker contributors | MIT | 2026-07-10 |
| `editor/resources/help/help.css` | original work (this repository) — generated by `help_style::css()`, checked in and gated by test | RoadMaker contributors | MIT | 2026-07-17 |
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
| `assets/library/props/sign_stop.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/sign_stop.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/sign_yield.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/sign_yield.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/streetlight_single.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/streetlight_single.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/streetlight_double.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/streetlight_double.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_low.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_low.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_mid.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_mid.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_tower.obj` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/props/building_tower.mtl` | original work (this repository), generated by `scripts/gen_prop_meshes.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/road_rural.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/road_urban.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/road_highway.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/style_urban.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/assembly_t.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/assembly_x.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/prop_tree_pine.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/prop_tree_oak.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/prop_tree_birch.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/prop_tree_poplar.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/prop_shrub.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/signal_traffic_light.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/signal_sign.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/sign_stop.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/sign_yield.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/streetlight_single.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/streetlight_double.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/building_low.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/building_mid.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/building_tower.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/marking_solid_white.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/marking_double_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/marking_dashed_white.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_dashed_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_double_white.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_solid_broken_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_broken_solid_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_double_dashed_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/marking_edge_white.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-18 |
| `assets/library/thumbnails/material_asphalt.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/material_asphalt_worn.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/material_concrete.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-17 |
| `assets/library/thumbnails/material_paint_white.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `assets/library/thumbnails/material_paint_yellow.png` | original work (this repository), generated by `scripts/gen_library_thumbnails.py` | RoadMaker contributors | MIT | 2026-07-19 |
| `docs/standards/golden-look.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/create-road.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/junction.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/elevation.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/library.png` | original work (this repository), editor screenshot | RoadMaker contributors | MIT | 2026-07-13 |
| `docs/user-guide/img/workflow.gif` | original work (this repository), scripted frame sequence via `scripts/editor_screenshot.py` (`--drag-ghost`/`--drop-library`) | RoadMaker contributors | MIT | 2026-07-21 |
| `docs/user-guide/img/gs1_hero.png` | original work (this repository), editor screenshot via `scripts/editor_screenshot.py` (same command as CI `visual-artifacts`) | RoadMaker contributors | MIT | 2026-07-21 |
| `docs/roadmap/archive/2026-07-pre-reset/golden_scenes/img/gs1_baseline_v0.6.0.png` | original work (this repository), viewport render from the GS-1 fixed camera, rendered by CI (`visual-artifacts`) | RoadMaker contributors | MIT | 2026-07-15 |
