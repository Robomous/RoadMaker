# Tools and the Library — how they work together

*RoadMaker has two ways to add things to a scene, and they are deliberately
different jobs. Understanding the split makes the editor predictable: **tools
are interaction modes, the Library is content.***

## The rule

- A **tool** on the toolbar is an *interaction mode* — a way of acting on the
  scene: draw a road, bend a node, form a junction, scatter props along a curve,
  paint a marking curve. A tool defines *how* you interact.
- A **Library** item is *content* — a specific asset you place: this road
  template, that crosswalk style, this tree, that material. The Library defines
  *what* you place.

Content workflows **start in the Library**. You pick the asset; the matching
tool does the placing.

## Two ways to place content

Every content asset can be added two ways, and both land as **one undoable
edit**:

1. **Drag it from the Library into the viewport.** A ghost preview follows the
   cursor so you see exactly where it lands; drop to place. This is the quickest
   path — one gesture, one command. See [Library](library.md).
2. **Select it in the Library to *arm* its tool.** Clicking an asset activates
   the tool that places it, with that asset current — then click in the viewport
   to place. This is the path when you want to place several, or place by
   clicking rather than dragging.

Selecting an asset arms:

| You pick in the Library… | …and this tool arms |
|---|---|
| a **prop** (tree, shrub, streetlight, building) | **Prop Point** |
| a **prop set** | **Prop Curve** (scatters the set along a path) |
| a **stencil** (arrow) | **Marking Point** |
| a **crosswalk** | **Crosswalk & Stop Line** |
| a **text sign** | **Sign** |
| a **road template** (on drop) | **Create Road** |

Assets with no single-placement tool — road **styles**, **materials**, plain
lane **markings**, and **assemblies** (T/X intersections) — are applied by
**dropping them onto their target** (a road, a lane, a boundary, a junction).
Selecting one instead opens it in the [Attributes](attributes.md) pane where it
can be edited.

## Why some tools look like Library items

A few toolbar tools overlap a Library asset on purpose — the tool is the *mode*,
the Library is the *content* that mode applies:

- **Create Junction** builds a T/X intersection interactively (snap two road
  ends, generate); the Library's **T/X assemblies** drop the same junction in
  one gesture. Use the tool to form a junction between existing roads; drop the
  assembly to stamp a standalone one.
- **Prop Point / Marking Point / Crosswalk / Sign** place one asset at a click.
  They use the asset you selected in the Library — so the Library is where you
  *choose*, and the tool is where you *place*.

Nothing is redundant: geometric and topology tools (roads, lanes, junctions,
corners, elevation, maneuvers) have no Library counterpart, and the content
placement tools take their asset from the Library.

## Finding content

The Library groups its catalogue by **category** — road templates, assemblies,
markings, materials, props, buildings, signals. Use the **category filter** at
the top of the dock to narrow the grid to one category, and the search box to
match by name. (A richer folder view over a project's asset files is planned —
see the roadmap's Library file explorer.)
