# P6 discovery — Assets, Props & Materials

*What the code actually looks like today, per P6 scope item, and what that
means for the sprint cut. Written 2026-07-17, before p6-s1 landed. Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-2](../golden_workflows/gw2_simple_scene.md) steps 1 and 16–20, plus
feeds into GW-3 and GW-5.*

## Why this document exists

The P6 sprint bodies were written from the roadmap, not from the code. Read
against the code, the pillar splits cleanly in two: the **plumbing is
further along than the plan assumed** — one MIME type already drags every
library kind into the viewport and into Attributes slots, and the kernel
already parses the repeat element the Prop Span tool needs — while the
**nouns are missing entirely**: there is no project, no thumbnail, no
material model, no prop tool, and no GPU instancing. P6 is roughly
**70% editor, 30% kernel**, the inverse of P2.

## 1. The Library — the drag model is done, the previews are not

The manifest (`assets/library/manifest.json`, v1) is a flat `items[]` of
`{key, label, category, thumbnail, create{kind,…}}` across five categories:
Road templates, Road styles, Assemblies, Props (four trees + shrub),
Signals. Loader `LibraryManifest` (`editor/src/document/library_manifest.*`)
maps five kinds and is forward-compatible (unknown version warns, unknown
kind → `Kind::Unknown`). The app loads it **from the qrc only**
(`:/library/manifest.json`, `main_window.cpp:433`); the from-disk
`LibraryManifest::load(path)` exists but nothing calls it — that unused
loader is exactly the seam a per-project asset overlay needs.

Drag-and-drop is already "one drag model everywhere":

| Path | State today |
|---|---|
| Viewport drop, every kind | **done** — `resolve_library_drop` (`library_drop.cpp`) handles template/style/assembly/tree/signal with per-kind snapping |
| Attributes-pane slots | **done for two** — Model slot (Props) and write-only Style slot (`properties_panel.cpp`); slot click auto-navigates the Library via `focus_category` |
| Markings onto a lane boundary | **missing** — no boundary drop target anywhere |
| Material onto a material slot | **missing** — no material slot, no material kind |
| Previews | **missing** — the panel decorates with themed `Icons::get` glyphs; `ThumbnailRole` exists in `LibraryListModel` but is never read, and the manifest's `assets/library/thumbnails/*.png` paths point at a directory that **does not exist** |

**Consequence:** p6-s2 is not "build a drag model" — it is (a) a thumbnail
pipeline plus wiring the already-present `ThumbnailRole`, and (b) new drop
*targets* (lane boundary, material slot) on the existing MIME contract.
P3's marking assets (#220) consume the boundary target, so it must land in
p6-s2, not wait for P3.

## 2. Project model — nothing exists; half of step 1's UX already does

There is no project, folder-of-assets, or workspace concept anywhere in
`editor/src` — a scene is a single `.xodr` and `Document::file_path_` is a
`QString`. But the *welcome-screen half* of GW-2 step 1 is already built:
`files/recent` in QSettings, recency-filtered on the welcome screen, with
per-scene thumbnails captured from the GL frame on save into
`AppData/thumbnails/<sha1>.png` (`main_window.cpp:919`,
`welcome_widget.cpp`). Autosave/recovery is Document-level and
project-agnostic.

**Adopted design (recommended default, per the standing auto-accept
directive):** a project is a **directory containing a `project.json`
manifest plus its scenes as ordinary `.xodr` files**, with an optional
`assets/` subfolder whose `library/manifest.json` overlays the built-in qrc
library at open (this is what the unused disk loader is for). No new scene
format, no database, no scene registry — scenes are discovered by glob, and
a scene stays openable standalone outside any project. QSettings grows
`files/recent_projects` next to `files/recent`.

## 3. Props — the kernel is ready, the tools are absent

Props are OpenDRIVE `<object>`s (`road/object.hpp`; `ObjectType` already
has Tree/Vegetation/Pole/Barrier/Building). Prop meshes are **procedural
and compiled in** (`scripts/gen_prop_meshes.py` →
`core/src/assets/prop_meshes.gen.cpp`; `props::ids()/model(id)`), consumed
identically by the mesher, both exporters, and the editor renderer. Edit
ops `add_object` / `move_object` / `remove_object` / `set_object_model`
exist, single-undo, restore-in-place.

- **Prop Point** is ~already the Library drag-drop (`add_object` snapped to
  a road); the tool form adds click-to-place-repeatedly. Smallest item in
  the pillar.
- **Prop Curve + Bake**: no curve distribution exists. Bake ≡ "N
  `add_object`s in one CompositeCommand", which the command layer supports
  today; the unbaked preview is a preview session like every P2 tool.
- **Prop Span**: the kernel **parses and round-trips `<repeat>`**
  (`ObjectRepeat`, `object.hpp:76-99` — s/length/distance, start/end
  t/z/width/height/radius, cubic t(ds)) but **zero editor code references
  it** and no edit op authors it. To verify at s5 start: whether the mesh
  builder expands a repeat into instances or ignores it — nothing in the
  survey confirmed either way, and the tool is pointless until the mesher
  renders what it authors.
- **Prop Polygon + Prop Sets**: nothing exists. A Prop Set is a
  library/manifest-level concept (multi-asset + portions), not a kernel
  type — the placed result is plain objects.

No prop tool ids exist (`ToolId` ends at LaneCarve). Each new tool pays the
usual editor cost but **no new selectable kind** — placed props are already
selectable/movable (`select_tool.cpp:124-197`) — so the ~17-site cost from
the P2 discovery does not recur.

## 4. Materials — RawXml passthrough is the promotion target

The kernel has **no material model**: `<material>` is Preserved-tier only
(`xodr/raw_xml.hpp` re-emits it verbatim). Mesh/export "materials" are
LaneType-derived colors shared across glTF/USD via `mesh_export_common.hpp`.
That matches the standing persistence decision (ADR'd during M&S planning):
**standard `<material>` plus ASAM `<userData>` inside the `.xodr`, no
sidecar; promotion from Preserved tier must keep RawXml** so unknown
attributes survive. p6-s3's kernel half is exactly that promotion, with
byte-stable round-trip tests.

The renderer half is **closer than planned**: `Material{base_color tint,
uv_scale, unlit}` with albedo sampling, two CC0 textures
(asphalt/concrete), `SurfaceKind` resolution and hemisphere+sun lighting
already run in `gl_renderer.cpp`. PBR-lite means adding normal+roughness
handles to that struct, two samplers to the one mesh shader, and a material
Library category — not a renderer rewrite.

## 5. Instanced rendering — scaffolding present, path unused

Issue #201's finding still holds verbatim: the draw loop
(`gl_renderer.cpp:620-633`) issues **one draw call per instance**, and the
comment says the instanced path "lands with prop instancing in a later PR"
— that PR is p6-s6. Worse, props/signals never even reach the instance
path: `scene_builder.cpp:150-218` **CPU-bakes every prop/signal part to
world space** as its own `SceneItem`, so `DrawItem::instances` (the
scaffolding, `renderer.hpp:51-68`) stays empty. A 1k-tree scene is
1k × parts meshes and as many draw calls.

The fix is well-bounded: scene_builder emits one shared mesh per prop model
part + per-instance transforms; gl_renderer uploads them as an instanced
VBO and calls `glDrawElementsInstanced`. glTF export already does
file-level instancing (shared mesh, node per instance) — the exporters
don't change.

## 6. Asset pipeline and content — the gap is content, not process

The pipeline is mature: `assets/manifest.json` ledger (url/sha256/license
per entry, `allowed_licenses = CC0/MIT/ISC/Apache-2.0`),
`fetch_assets.py`, and a CI lint (`check_asset_licenses.py`) that fails on
any unledgered file under `assets/` or `editor/resources/`. What is
*missing* is content: the only props are five procedural plants and two
procedural signal meshes; there are no buildings, streetlights, stencils,
marking or material assets, and only two textures. **Content curation
(p6-s6) requires human license judgment per the assets policy — it is not
autonomous-safe and needs the maintainer in the loop**, unlike every other
P6 item.

## GW step ownership

P6 owns GW-2 **steps 1 and 16–20**, provides the material-slot plumbing
step 15 uses (P3 owns the step), and feeds GW-3 and GW-5 through the
Library. Step 1's expected text matches the adopted project design (folder
of shared assets; recents on the welcome screen).

## Sprint cut this implies

| Sprint | Reality-adjusted scope |
|---|---|
| [#235](https://github.com/Robomous/RoadMaker/issues/235) p6-s1 | Smaller than written: recents + scene thumbnails already exist. Real scope = `project.json` folder model (§2), New/Open Project flows, recent-projects, per-project library overlay via the unused disk loader |
| [#236](https://github.com/Robomous/RoadMaker/issues/236) p6-s2 | Thumbnail pipeline + wire the dormant `ThumbnailRole` (§1); new drop targets: lane boundary (P3's dependency) + material slot; categories for materials/markings |
| [#237](https://github.com/Robomous/RoadMaker/issues/237) p6-s3 | Kernel: promote `<material>`+`<userData>` from Preserved tier (keep RawXml, byte-stable). Renderer: extend the existing `Material`/shader with normal+roughness (§4) |
| [#238](https://github.com/Robomous/RoadMaker/issues/238) p6-s4 | Prop Point (small) + Prop Curve with Bake-as-CompositeCommand (§3); preview sessions per the P2 pattern |
| [#239](https://github.com/Robomous/RoadMaker/issues/239) p6-s5 | Prop Span = author the already-round-tripping `ObjectRepeat` (verify mesher expansion first, §3); Prop Polygon + Prop Sets (manifest-level) |
| [#240](https://github.com/Robomous/RoadMaker/issues/240) p6-s6 | Instanced fast path (§5, well-bounded); CC0 content curation — **maintainer-in-the-loop**, the pillar's only non-autonomous item |

Sequencing: **s1 → s2** first (s2 unblocks P3), then s3 ∥ s4 → s5 → s6.
The instancing half of s6 can land any time after s4 makes prop counts
grow; the content half is independent and human-gated.
