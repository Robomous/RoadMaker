# Edit Nodes

*Reshape an existing road by moving, inserting, and deleting the waypoints its
reference line was fitted through.*

## Steps

1. Select a road, then activate the **Edit Nodes** tool. The road's waypoint
   handles and tangent visualization appear.
2. **Move** a node: drag its handle. The clothoid is refitted through the moved
   waypoint on release — one command per drag.
3. **Insert** a node: click on the reference line between two nodes. A new
   waypoint is added where you clicked; the shape is preserved until you move
   it.
4. **Delete** a node: select it and press <kbd>Delete</kbd>.

Each insert or delete is a single undoable command; a drag is a preview
session that commits one command on release.

## Notes

- Roads loaded from a foreign `.xodr` carry no stored waypoints — Edit Nodes
  derives them from the geometry the first time you use it, so you can reshape
  imported roads too.
- Moving a node of a road that feeds a junction regenerates the junction's
  connecting lanes ([Junction](junction.md)).

## Reference

[M2 editing tools §3 (Edit Nodes)](../design/m2/02_editing_tools.md) — insert /
delete / drag semantics and the `edit::insert_waypoint` /
`edit::delete_waypoint` kernel API.
