# RoadMaker user guide

*Task-by-task guidance for authoring an ASAM OpenDRIVE road network in the
RoadMaker editor — how to build a scene, not a reference manual.*

New here? Start with [Building](../getting-started/building.md) and
[Running](../getting-started/running.md) to get the editor and the Python
package installed, then come back.

This guide is deliberately short: each page covers one tool with the steps to
get a useful result. For the precise, normative behaviour of a tool (event
sequences, undo semantics, kernel API), follow the linked design spec on each
page.

## The editor at a glance

Launch `roadmaker-editor` with an optional `.xodr` argument
([Running → The editor](../getting-started/running.md#the-editor)). The window
has four parts:

- **3D viewport** — the road mesh, right-handed and Z-up. Click to pick roads
  and lanes; the active tool interprets clicks and drags.
- **Scene tree** — every road, lane, and junction in the network.
- **Properties panel** — the fields of the current selection; editing a field
  is an undoable command.
- **Diagnostics panel** — parser and validator findings, each citing its ASAM
  rule id where one applies.

Every edit goes through the command layer, so **Undo/Redo** (the single undo
stack) reverses anything in this guide. Selection itself is view state and is
not undoable.

## Tools

| Tool | Task |
|---|---|
| [Create Road](create-road.md) | Lay a new clothoid road through waypoints with a lane template |
| [Edit Nodes](edit-nodes.md) | Reshape a road by moving, inserting, and deleting its waypoints |
| [Lane Profile](lane-profile.md) | Add, remove, and retype lanes across a road's cross-section |
| [Elevation](elevation.md) | Give a road a vertical profile |
| [Junction](junction.md) | Connect road ends into a junction with generated turning lanes |
| [T-junction](t-junction.md) | Tee a road's end into another road's body |
| [Objects & signals](objects-signals.md) | Add crosswalks, props, traffic lights, and signs |
| [Save & export](save-export.md) | Write OpenDRIVE and export meshes (glTF, USD) |

## About the screenshots

This first edition of the guide is text-only. Per-tool screenshots recorded
against the v0.4.0 build are captured as original works and listed in
`ASSETS_LICENSES.md` as they are added — the in-editor object/signal placement
and library-panel walkthroughs land alongside their editor features later in
milestone M3a.

## See also

- [Running RoadMaker](../getting-started/running.md) — launching the editor,
  the sample files, the Python package
- [OpenDRIVE conventions](../domain/opendrive.md) — s/t coordinates, lane
  numbering, the terms this guide uses
- [Architecture overview](../architecture/overview.md) — how the kernel,
  Python bindings, and editor fit together
