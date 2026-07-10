# roadmaker (Python)

Python bindings for the RoadMaker kernel: ASAM OpenDRIVE reading/writing,
clothoid road authoring, mesh generation, and glTF export.

```python
import roadmaker as rm

network, diagnostics = rm.load_xodr("road.xodr")
mesh = rm.build_network_mesh(network)
rm.export_glb(mesh, "road.glb")
```

Part of the [RoadMaker](https://github.com/robomous/roadmaker) project (MIT).
