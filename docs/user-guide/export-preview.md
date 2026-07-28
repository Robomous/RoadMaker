# Export previews

*See what an export will contain — and what it will leave behind — before you
write a single file.*

Two tools, both under **File**, both read-only: they never change your scene,
never write a file, and never mark the document dirty.

- **File → Scene Export Preview…** — what the 3D export (glTF or OpenUSD) would
  contain.
- **File → OpenDRIVE Export Preview…** — the `.xodr` exactly as it would be
  written, with the checker's findings.

They share one window with a page each, so you can move between them.

## Scene Export Preview

The table has a row for every channel of the scene, and it deliberately shows
the empty and the omitted ones too — a channel that quietly vanishes from a
report is how missing ground went unnoticed from the day it shipped.

| Column | Meaning |
|---|---|
| **In scene** | how many records the scene holds |
| **In file** | how many the export would actually carry |
| **Triangles** / **Vertices** | what the *file* stores |
| **Status** | `Exported`, `Not written`, `Not supported by this format`, or `Partly written` |

Below it, the materials the exporter would write — with the colour and
roughness it would give each — and the scene's extents.

### Why the two formats disagree

Switching **Format** between glTF and OpenUSD changes the triangle count, and
that is correct. glTF stores one copy of each prop model and places it with a
node per instance; OpenUSD bakes every instance's geometry into the file. An
avenue of two hundred identical trees is one tree's geometry in a `.glb` and
two hundred trees' in a `.usda`.

If the build you are running cannot write `.usda`, the page says so and shows
the manifest anyway — it is still an accurate description of what a
USD-enabled build would produce.

### What is not exported today

The preview names these rather than hiding them:

- **OpenUSD cannot carry sign-face text.** Sign bodies export normally; the
  rasterised legend does not
  ([#364](https://github.com/Robomous/RoadMaker/issues/364)). glTF embeds it.
- **A placement naming a model this build does not ship is skipped silently by
  the exporter.** The preview counts them.

Ground surfaces and the terrain field used to be on this list: for two pillars
neither exporter wrote them, so a scene with a height field exported without its
ground. Both now write both channels
([#390](https://github.com/Robomous/RoadMaker/issues/390)) — a surface takes a
`ground_<material>` material (plain grass when it carries none) and the field
takes `ground_terrain`, in the same colours the viewport draws them.

### "Nothing would be exported"

The exporters refuse only a scene whose every channel is empty. Terrain, ground
surfaces, bridges or props on their own are real geometry and export fine. The
preview shows that verdict up front instead of letting you meet it at a save
dialog.

## OpenDRIVE Export Preview

The summary line counts roads, reference-line length, junctions, lane sections,
lanes, objects and signals — all read back out of the document the writer
produced, so it describes the file rather than an idea of it. Below it:

- **RoadMaker extensions carried in the file** — the `rm:` records that travel
  inside `<userData>`. They are the ASAM-adjacent layer described in
  [ADR-0008](../decisions/0008-persistence-layers-asam-first.md): another tool
  ignores them and still reads the file. Your camera and render mode are *not*
  here, and never will be — those live in the scene's `.rmscene.json`
  companion.
- **The OpenDRIVE itself**, read-only, exactly as it would be written.

If a terrain sidecar would be written beside the `.xodr`, the summary names it.

### Diagnostics

Opening this page runs the OpenDRIVE checker over the network **as it stands
now** and republishes the results to the [Diagnostics](diagnostics.md) panel,
each finding citing its ASAM rule where one applies. Before this, the checker
only ran when you saved — so the panel described the file you had opened rather
than the network you were building.

A file the writer refuses is a special case worth knowing about: the refusal
itself is a single message about the first defect it hit, so the summary tells
you how many findings there are in total and sends you to the Diagnostics panel
for the rest.

## The advisories, by rule

Each thing the preview reports about an export carries a citable rule id, in
the RoadMaker vendor namespace — ASAM has no equivalent, since these describe
*this tool's* exporters rather than the standard. They are advisory and never
block anything.

| Rule UID | Fires when |
|---|---|
| `robomous.ai:rm:1.0.0:export.channel_not_written` | A channel holds geometry that the chosen exporter does not walk at all. No channel is in this state today — the ground channels were the last (#390) — but the rule stays, so a channel that ever ships unexported announces itself. |
| `robomous.ai:rm:1.0.0:export.format_unsupported` | The format cannot carry something the scene holds — today OpenUSD and sign-face textures (#364). |
| `robomous.ai:rm:1.0.0:export.model_unresolved` | A placement names a prop or signal model this build does not ship; the exporter skips it silently. |
| `robomous.ai:rm:1.0.0:export.nothing_to_export` | The exporters would refuse the scene outright, because every channel is empty. |

## From Python

Both previews are kernel API, so a headless pipeline can ask the same
questions:

```python
import roadmaker as rm

network, _ = rm.load_xodr("scene.xodr")
mesh = rm.build_network_mesh(network)

scene = rm.preview_mesh_export(mesh, rm.MeshExportFormat.GLTF)
print(scene.total_triangles, len(scene.materials))
for row in scene.channels:
    if row.reason != rm.OmissionReason.NONE and row.elements:
        print("not fully written:", row.label, "-", row.detail)

xodr = rm.preview_xodr_export(network, "scene")
print(xodr.would_write, xodr.byte_count, len(xodr.diagnostics))
```

`rm.mesh_export_available(fmt)` reports whether the build can write a format;
the preview is computed either way.

See [`python/examples/export_preview.py`](https://github.com/Robomous/RoadMaker/blob/main/python/examples/export_preview.py)
for a runnable version.
