# Merge & Split Roads

*Cut one road into two, or weld two adjacent roads back into one.*

## Split a road

1. Activate the **Split** tool (scissors icon, `K`). Hover a road — a cut
   marker follows the nearest point on it.
2. **Click** to split the road there. Both halves are selected and named in the
   status bar, and the tool returns to Select. The split is one undo step.

You can also right-click a road body and choose **Split road here**, or
right-click a node and choose **Split at this node**.

## Merge two roads

1. **Select exactly two roads** that meet end-to-start (the end of one touches
   the start of the other) with matching lane profiles and elevation — for
   example the two halves of a road you just split.
2. Trigger **Merge** (the git-merge toolbar button, **Edit → Merge Roads**, or
   **Merge selected roads** in a road's right-click menu). The two become one
   road that keeps the first road's id; the second is removed. One undo step.

The **Merge** command is enabled only when the selection can actually merge.
When it can't, the reason is shown, one of:

- > road *N* or *M* participates in a junction — merging junction roads isn't
  > supported yet; delete the junction first
- > road *N*'s end is already connected to another road
- > the roads meet end-to-end — reverse one first (coming soon)
- > the joining ends are *X* m apart — move them together first
- > the joining ends' headings differ — align them first
- > lane *N* … doesn't match at the seam (width / type / road mark), or the lane
  > offset / elevation doesn't match

## Notes

- **Split then merge is lossless in shape**: merging concatenates the sections
  rather than coalescing them, so the road evaluates identically to the
  original even though an extra section boundary remains at the old seam.
- Undo restores the pre-merge (or pre-split) network byte-for-byte.

## Not yet

Reversing a road so end-to-end roads can merge, and merging roads through a
junction, are follow-ups.

## Reference

The `edit::split_road` / `merge_roads` / `check_mergeable` kernel API and the
merge precondition list are documented in the M3a topology-editing design notes
(`docs/design/m3a/06_topology_editing.md`).
