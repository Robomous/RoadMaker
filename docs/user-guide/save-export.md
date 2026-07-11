# Save & export

*Write your network back to OpenDRIVE, and export meshes for rendering and
downstream tools.*

## Save OpenDRIVE

Use **File → Save** (or New / Open) to write the network as `.xodr`. The writer
is version-explicit: it targets a chosen OpenDRIVE revision and emits only the
attributes valid for it, so a saved file always validates at its declared
version. The validator's findings appear in the diagnostics panel, each citing
its ASAM rule id where one applies.

Saving is a fixed point: opening a file and saving it again is byte-stable, and
anything RoadMaker does not model (foreign attributes and elements) is
preserved verbatim rather than dropped.

## Export meshes

From the Python package you can export the tessellated network:

- **glTF** (`.glb`) — `rm.export_glb(network, "road.glb")`
- **OpenUSD** (`.usda`) — the USD exporter (see
  [USD export design](../design/m2/04_usd_export.md))

```python
import roadmaker as rm

network, diagnostics = rm.load_xodr("assets/samples/t_junction.xodr")
rm.export_glb(network, "t_junction.glb")
```

Meshing converts the kernel's Z-up frame to Y-up only at the glTF boundary; the
network model itself stays right-handed and Z-up.

## Reference

- [Running → Python package](../getting-started/running.md#python-package) —
  load / author / export from code.
- [OpenDRIVE conventions](../domain/opendrive.md) — what the writer emits and
  the coordinate frame it uses.
