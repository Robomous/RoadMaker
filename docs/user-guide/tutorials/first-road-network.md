# Your first road network

*Start from an empty window and finish with a two-road T-junction written out as
OpenDRIVE. About ten minutes, no prior setup.*

![A T-junction: one road teed into the side of another, shown in the editor
viewport](img/first-road-network.png)

## Before you start

Launch `roadmaker-editor` with no arguments so you get an empty scene. If you
have never moved the camera before, read [Getting around](getting-around.md)
first — a minute of orbiting and framing makes everything below easier.

## 1. Draw the first road

Pick the **Create Road** tool from the toolbar. In the tool-options row, choose a
cross-section template (start with **Urban** for a two-lane road). Click once in
the viewport to drop the start point, click again further along for a waypoint,
and **double-click** to finish the road. You now have a single clothoid road with
lanes.

If the shape is not quite right, switch to **Edit Nodes** and drag the waypoints;
see [Edit Nodes](../edit-nodes.md) for inserting and deleting them.

## 2. Tee in a second road

Draw a second road with **Create Road** so that its end reaches the side of the
first. To connect them, use the **T-junction** flow: bring the second road's end
onto the first road's body and let the editor form the tee. The
[T-junction](../t-junction.md) page walks through the exact gesture and what the
generated connecting lanes look like.

Prefer a full intersection instead of a tee? Draw both road ends into the same
area and use the [Junction](../junction.md) tool to connect them with generated
turning lanes.

## 3. Check it

Open the **Scene** panel to confirm both roads and the junction are present, and
glance at the [Diagnostics](../diagnostics.md) panel — a clean network raises no
errors. Every step so far is undoable with <kbd>Ctrl</kbd>+<kbd>Z</kbd>.

## 4. Export OpenDRIVE

Choose **File ▸ Save** to write a `.xodr`. That file is standards-correct
OpenDRIVE you can load into any compliant tool. To export a mesh instead (glTF or
USD), see [Save & export](../save-export.md).

## Where to go next

- [Shaping lanes](shaping-lanes.md) — add and retype lanes on the roads you just
  drew.
- [Working with road styles](working-with-road-styles.md) — restyle a whole road
  in one drop.
