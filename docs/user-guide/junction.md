# Junction

*Connect the ends of several roads into a junction, with turning lanes
generated between every arm.*

![A four-arm junction with generated turning lanes](img/junction.png)

## Steps

1. Arrange the roads so their ends meet where the junction should be.
2. Activate the **Junction** tool and select the road **ends** (arms) to join —
   two or more.
3. Create the junction. RoadMaker:
   - generates a **connecting road** for each permitted turn between arms,
     matching driving lanes curb-in;
   - records the connections and per-lane links;
   - blends a **2.5D surface** across the junction from the arm elevations;
   - derives a counter-clockwise, closed junction **boundary** for export.

The junction is one undoable command. Moving an arm road later (via
[Edit Nodes](edit-nodes.md)) regenerates the connecting lanes.

To attach a road's end to another road's **body** instead of joining ends,
see [T-junction](t-junction.md) — same tool, one selected end plus a body
anchor.

## Notes

- The selected arms are remembered on the junction, so it can be regenerated
  after edits; junctions from foreign files load without this list and cannot
  regenerate until recreated.
- The sample `assets/samples/t_junction.xodr` is a ready-made three-road
  junction to open and inspect
  ([Running → Sample files](../getting-started/running.md#sample-files)).

## Reference

[M2 editing tools §6 (Create Junction)](../design/m2/02_editing_tools.md) and
[junction blending](../design/m2/03_junction_blending.md) — connecting-road
generation, the blended surface, and the exported reference line / elevation
grid / boundary.
