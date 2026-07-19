# Library

*Drag ready-made roads, intersections, and props from a searchable catalogue
straight into the scene — every drop is one undoable edit.*

![The Library dock open beside a T-junction lined with tree props](img/library.png)

The **Library** is a dock (tabbed with the Scene tree by default). Open it from
the **Library** button on the toolbar, from **Edit ▸ Add from Library…**, or by
clicking its tab. Type in the search box to filter the catalogue by name. Every
item shows a **preview thumbnail** — props and signals render their actual 3-D
model, while templates, markings, and materials show a stylised swatch.

## What's in it

| Category | Items | Drops as |
|---|---|---|
| **Road templates** | 2-lane rural, urban with sidewalks, 4-lane divided | a road with that lane template |
| **Road styles** | Urban 2-lane | re-styles the road you drop it on |
| **Assemblies** | T-intersection, X-intersection | a pre-built junction |
| **Buildings** | Low block, Mid-rise, Tower | an OpenDRIVE `<object>` (typed `building`) on the nearest road |
| **Props** | Pine / Oak / Birch / Poplar tree, Shrub, Streetlight (single / double), City block set | an OpenDRIVE `<object>` on the nearest road — trees are typed `tree`, streetlights `pole`; the set scatters a mix of buildings |
| **Signals** | Traffic light, Traffic sign, Stop sign, Yield sign | an OpenDRIVE `<signal>` on the nearest road — the light is a dynamic control; the signs are static (Stop = DE 206, Yield = DE 205) |
| **Markings** | Solid single white, Double yellow, Dashed white / yellow, Double white, Solid–broken / Broken–solid yellow, Double dashed yellow, Wide edge white | a `<roadMark>` on the lane boundary you drop it on |
| **Materials** | Asphalt, Asphalt (worn), Concrete, Paint (white / yellow) | a `<material>` on the lane (or ground surface) you drop it on ([materials](materials.md)) |

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

## Markings drop on a lane boundary

Drag a **marking** onto the viewport and drop it near a lane edge: RoadMaker
snaps to the **nearest lane boundary** in the lane section under the cursor and
paints it there (a solid line on a lane edge, a double yellow on the centre
line). The ghost sits on the boundary while you drag, so what you see is where
it lands. Drop one away from any road and the editor places nothing and hints to
drop it onto a boundary; dropping the same marking a boundary already carries is
a no-op. A marking is a lane's outer-boundary `<roadMark>` — see
[lane markings](lane-form.md).

The Markings catalogue covers the common line styles: single solid, dashed
(broken), the double families (double white/yellow, double dashed, and the
solid–broken and broken–solid combinations), and a wide edge line. You can also
drop a marking onto the **Marking** slot in the Properties panel to set the
selected lane's road mark without aiming in the viewport — see
[lane profile](lane-profile.md).

## Materials drop on a surface slot

A **material** re-textures a [ground surface](../user-guide/attributes.md). It
has no world point to snap to, so it is dropped onto the **Materials** slot in
the Attributes pane of a selected surface (dropping one on the viewport hints
you there). See [attributes](attributes.md#ground-surface-material).

## See also

- [Objects & signals](objects-signals.md) — the full object/signal model props
  belong to.
- [UI revamp — Library dock](../design/ui-revamp/phase2_library.md) and
  [props](../design/ui-revamp/phase3_props.md) — the design specs.
