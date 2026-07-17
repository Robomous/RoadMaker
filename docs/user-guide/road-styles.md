# Road Styles

*Restyle a whole road in one drop — a road style replaces the cross-section and
its markings while preserving everything orthogonal to it.*

![The editor with a road selected and a road style dragged from the Library
onto it](img/road-styles.png)

## Steps

1. Open the **Library** dock and find the **Road styles** category (e.g. *Urban
   2-lane*).
2. **Drag** a style onto an existing road in the viewport. A ghost previews the
   drop while you drag.
3. **Drop**. The style replaces the road's lane profile and boundary markings,
   flattening the road to a single lane section.

Applying a style is one undoable command. It refuses a connecting road (inside a
junction) or a style that defines no lanes.

## The replace-vs-preserve contract

A road style *defines the cross-section*, so applying one **replaces**:

- the lane profile (lane count, types, and widths), and
- the boundary road markings.

Everything **orthogonal** to the cross-section **survives** unchanged:

- the reference-line geometry (the road's shape in plan),
- the elevation and superelevation profiles,
- junction connectivity and lane links,
- the road's name, and
- any objects or signals already placed on it.

This is why a style is a *restyle*, not a rebuild: drop *Urban 2-lane* onto a
road you have already raised over a crossing and placed trees along, and the
bridge and the trees stay put — only the lanes and their markings change.

## Notes

- Road styles are a distinct Library category from **Road templates**. A
  template is a lane profile you draw a *new* road with (see
  [Create Road](create-road.md) and [Library](library.md)); a style is dropped
  onto an *existing* road to restyle it.
- To change lanes individually rather than wholesale, use the
  [Lane](lane-profile.md), [Lane Add](lane-add.md), [Lane Form](lane-form.md),
  and [Lane Carve](lane-carve.md) tools.

## Reference

[Library](library.md) for the drag-and-drop model and the
[P2 discovery report](../roadmap/pillars/p2_discovery.md) for the road-style
preservation contract.
