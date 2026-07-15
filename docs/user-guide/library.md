# Library

*Drag ready-made roads, intersections, and props from a searchable catalogue
straight into the scene — every drop is one undoable edit.*

![The Library dock open beside a T-junction lined with tree props](img/library.png)

The **Library** is a dock (tabbed with the Scene tree by default). Open it from
the **Library** button on the toolbar, from **Edit ▸ Add from Library…**, or by
clicking its tab. Type in the search box to filter the catalogue by name.

## What's in it

| Category | Items | Drops as |
|---|---|---|
| **Road templates** | 2-lane rural, urban with sidewalks, 4-lane divided | a road with that lane template |
| **Assemblies** | T-intersection, X-intersection | a pre-built junction |
| **Props** | Pine / Oak / Birch / Poplar tree, Shrub | an OpenDRIVE `<object type="tree">` on the nearest road |
| **Signals** | Traffic light, Traffic sign | an OpenDRIVE `<signal>` on the nearest road — the light is a dynamic control, the sign a static one |

## Placing an item

1. Open the Library and find the item (search or scroll).
2. **Drag** it into the viewport. A ghost preview follows the cursor so you can
   see what will be placed and where.
3. **Drop** it. RoadMaker creates the geometry as a single command — **Undo**
   (⌘Z / Ctrl-Z) removes it in one step.

## Props and signals snap to a road

Props and signals are ASAM OpenDRIVE **objects** and **signals**, which always
live on a road (in road `s`/`t` coordinates —
[objects & signals](objects-signals.md)). When you drop a tree, a traffic
light, or a sign, RoadMaker snaps it to the **nearest road** within a short
threshold and records its position along that road. Drop one with no road
nearby and the editor places nothing and shows a hint to drop it closer to a
road — so it never ends up floating with no reference line.

Placed props and signals are selectable and deletable like any other entity
(click to select; **Delete**; right-click for **Delete / Frame / Duplicate**),
and they round-trip through save/reload and into the glTF and USD exports
([save & export](save-export.md)). A selected signal's road-relative pose
(`s` / `t` / heading offset) edits in the properties panel — see
[objects & signals](objects-signals.md).

## See also

- [Objects & signals](objects-signals.md) — the full object/signal model props
  belong to.
- [UI revamp — Library dock](../design/ui-revamp/phase2_library.md) and
  [props](../design/ui-revamp/phase3_props.md) — the design specs.
