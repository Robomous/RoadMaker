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
report is how missing ground went unnoticed for two release cycles.

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

- **Ground surfaces and terrain are written by neither exporter.** A scene with
  a height field or an enclosed ground surface exports without it
  ([#390](https://github.com/Robomous/RoadMaker/issues/390)).
- **OpenUSD cannot carry sign-face text.** Sign bodies export normally; the
  rasterised legend does not
  ([#364](https://github.com/Robomous/RoadMaker/issues/364)). glTF embeds it.
- **A placement naming a model this build does not ship is skipped silently by
  the exporter.** The preview counts them.

### "Nothing would be exported"

Both exporters refuse a scene with no roads and no junction floors — even one
that holds terrain, ground surfaces, bridges or props. The preview shows that
verdict up front instead of letting you meet it at a save dialog.

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
