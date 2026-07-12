# T-junction (tee into a road body)

*Attach the end of one road to the SIDE of another — the second thing anyone
draws after their first road. One command splits the target, carves out the
junction area, and generates all legal turns.*

## Steps

1. Draw the main road, then draw the side road ending near the main road's
   body (or reuse an existing road with a free end).
2. Activate the **Junction** tool (`J`) and click the side road's **end** —
   exactly one end selected.
3. Click the main road's **body** (away from its endpoints). The viewport
   shows the tee preview:
   - an **anchor marker** on the main road's reference line at the projected
     station `s` (the status bar and the viewport hint show the value);
   - a **dashed ghost line** from your selected end to the anchor;
   - a highlighted span with end ticks — the stretch `[s−gap, s+gap]` of the
     main road that the junction area will **replace**.
4. Press **Enter**. RoadMaker splits the main road around the anchor, removes
   the middle piece (it becomes the junction area), trims the side road back
   to the junction boundary if it overhangs, and generates connecting roads
   for **all legal turns** — through movements included.
5. **Esc** cancels at any point before Enter; afterwards, a single **Undo**
   restores everything byte-identically.

## Gap auto-sizing

The half-length `gap` of the replaced span is derived automatically from the
larger of:

- the **width bound** — the junction area must at least span the crossing
  road's body (max half-width of the two roads + 1 m), and
- the **turning bound** — every generated turn must stay drivable at the
  minimum turn radius (6 m by default): a turn deflecting by Δθ needs
  `r·tan(Δθ/2)` of clearance along each leg. Shallow-angle tees therefore
  get a longer junction area than perpendicular ones, and attaching on the
  inside of a curve gets more room than the outside.

The side road needs the same clearance: if its end reaches past the junction
boundary, the overhang is trimmed into the junction area (undo restores it).
If the side road is too short to reach, the attach reports an error instead.

## Notes

- Endpoint clicks always win over the body anchor — clicking a road end
  toggles it into an endpoint-junction selection; a tee needs exactly ONE
  selected end plus a body anchor.
- The main road can be teed into repeatedly (each attach splits the current
  piece).
- The sample `assets/samples/t_attach.xodr` is a ready-made tee to open and
  inspect.

## Reference

[T-junction design](../design/hardening/t_junction.md) — the split/attach
composition, the gap formula, and the connecting-road conventions with their
OpenDRIVE rule citations.
