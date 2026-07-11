# Create Road

*Lay a new road through a series of waypoints; RoadMaker fits a smooth
clothoid reference line and applies a lane template.*

## Steps

1. Select the **Create Road** tool from the toolbar.
2. Pick a **lane template** from the toolbar dropdown (the kernel `LaneProfile`
   presets — e.g. a two-lane rural road or an urban road with sidewalks).
3. Click in the viewport to drop **waypoints** along the path you want. A live
   preview shows the reference line and lane edges as you go; nothing is
   committed to the network yet.
4. Finish the road (double-click / press Enter). RoadMaker fits a
   G1-continuous clothoid through the waypoints, builds the lane sections from
   the template, and commits the road as a single undoable command.

The waypoints are remembered on the road, so [Edit Nodes](edit-nodes.md) can
reshape it later without re-deriving them.

## Notes

- The reference line is arc length `s` in metres, right-handed and Z-up.
  Lane ids follow OpenDRIVE: negative to the right of the reference line,
  positive to the left, `0` the centre line — see
  [OpenDRIVE conventions](../domain/opendrive.md).
- Prefer to author from code? `python/examples/author_road.py` builds the same
  clothoid road through waypoints and writes OpenDRIVE + glTF
  ([Running → Runnable examples](../getting-started/running.md#runnable-examples)).

## Reference

Precise interaction, preview, and undo semantics:
[M2 editing tools §2 (Create Road)](../design/m2/02_editing_tools.md).
