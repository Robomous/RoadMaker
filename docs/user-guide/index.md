# RoadMaker user guide

*Task-by-task guidance for authoring an ASAM OpenDRIVE road network in the
RoadMaker editor — how to build a scene, not a reference manual.*

![The RoadMaker editor: a T-junction lined with tree props, the Library
catalogue open](../standards/golden-look.png)

First launch runs a short, skippable **guided tour** of the core loop (draw a
road → drag in an intersection → plant a tree → shape the elevation → export);
replay it any time from **Help ▸ Guided Tour**.

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
| [Camera & navigation](camera-navigation.md) | Orbit, zoom, and pan the viewport around its pivot |
| [Keyboard shortcuts](shortcuts.md) | Every binding, generated from the editor's own table |
| [Editing attributes](attributes.md) | Scrub numeric fields by dragging their name; drop assets into slots |
| [Scene tree](scene-tree.md) | Find and select any road, lane, or junction from the structural list |
| [Diagnostics](diagnostics.md) | Read parser and validator findings, each citing its ASAM rule |
| [Library](library.md) | Drag ready-made roads, intersections, and props into the scene |
| [Projects](projects.md) | Group scenes and shared Library assets in a project folder |
| [Create Road](create-road.md) | Lay a new clothoid road through waypoints with a lane template |
| [Moving and transforming](moving-and-transforming.md) | Move and rotate roads and props with the Move tool and its 3D transform gizmo |
| [Edit Nodes](edit-nodes.md) | Reshape a road by moving, inserting, and deleting its waypoints |
| [Bend Points](bend-points.md) | Double-click a road to add a bend node without reshaping it |
| [Merge & Split](merge-split.md) | Cut a road in two, or weld two adjacent roads into one |
| [Right-Click Menus](context-menus.md) | The context actions for roads, nodes, junctions, and empty space |
| [Lane](lane-profile.md) | Add, remove, and retype lanes and set travel direction across a road's cross-section |
| [Lane Width](lane-width.md) | Shape a lane's width along the road as a 2D curve |
| [Lane Add](lane-add.md) | Drop a self-contained pocket lane into the middle of a road |
| [Lane Form](lane-form.md) | Grow a new lane from a point to the road's end, linked across seams |
| [Lane Carve](lane-carve.md) | Carve a tapering turn lane on a junction approach |
| [Road Styles](road-styles.md) | Restyle a whole road by dropping a style from the Library |
| [Elevation](elevation.md) | Give a road a vertical profile |
| [Junction](junction.md) | Connect road ends into a junction with generated turning lanes |
| [T-junction](t-junction.md) | Tee a road's end into another road's body |
| [Objects & signals](objects-signals.md) | Add crosswalks, props, traffic lights, and signs |
| [Road markings](markings.md) | Place arrow stencils on a lane and draw free-form line markings and crossings |
| [Materials](materials.md) | Pave a lane or surface with asphalt or concrete, textured and saved into the file |
| [Textured rendering](textured-rendering.md) | Switch the viewport between the Sober working look and the daytime textured look |
| [Viewport hints](viewport-hints.md) | Show or hide the active tool's hint card in the viewport corner |
| [Save & export](save-export.md) | Write OpenDRIVE and export meshes (glTF, USD) |

## Tutorials

End-to-end walkthroughs that string the tools above together. Each one builds a
small scene from an empty window and points at the tool pages for the details.

| Tutorial | You will |
|---|---|
| [Your first road network](tutorials/first-road-network.md) | Draw a road, tee in a second, and export OpenDRIVE |
| [Shaping lanes](tutorials/shaping-lanes.md) | Add, retype, widen, and pocket lanes across a road |
| [Working with road styles](tutorials/working-with-road-styles.md) | Restyle a whole road by dropping a style from the Library |
| [Getting around](tutorials/getting-around.md) | Orbit, frame, and switch views to work quickly |

## About the screenshots

Screenshots are captured from the themed (graphite-amber) editor with
`scripts/editor_screenshot.py` as original works of this project. The Library,
Create Road, Junction, Elevation, Objects, Lane, Lane Width, Lane Add, Lane
Form, Lane Carve, and Road Styles pages carry them today; the remaining tool
pages gain theirs as the sweep continues
([#52](https://github.com/Robomous/RoadMaker/issues/52)).

## See also

- [Running RoadMaker](../getting-started/running.md) — launching the editor,
  the sample files, the Python package
- [OpenDRIVE conventions](../domain/opendrive.md) — s/t coordinates, lane
  numbering, the terms this guide uses
- [Architecture overview](../architecture/overview.md) — how the kernel,
  Python bindings, and editor fit together
