# M3a · Topology editing UX (as-built)

The opening M3a work adds the missing interactive topology operations — moving
whole roads, inserting bend points, splitting, and merging — plus a natural
middle-mouse pan and a cross-cutting right-click context-menu system. Every
mutation goes through the M2 command layer (one `QUndoCommand` per edit,
`apply→revert` leaves `write_xodr` byte-identical) and every controller is
headless-testable.

Shipped across six PRs: MMB pan · move whole road · bend points · split +
context-menu core · merge · context menus complete + this doc.

## Kernel operations

| Factory | Effect | Undo |
|---|---|---|
| `translate_roads(net, ids, dx, dy)` | shift plan-view + waypoints of N roads as one command | byte-identical |
| `insert_node_at(net, road, s)` | add a node at `s`, headings pinned from the current curve | byte-identical |
| `split_road(net, road, s)` | (M2) cut into two linked roads | byte-identical |
| `merge_roads(net, a, b)` | weld a's END into b's START, keep a's id | byte-identical |
| `check_mergeable(net, a, b)` | non-mutating merge precondition / enablement query | — |

`translate_road` and `translate_roads` shift every `GeometryRecord` and every
authoring waypoint by `(dx, dy)`; headings, lengths, s-values, lanes, elevation
and marks are untouched, so undo is byte-identical from the value snapshots.
`insert_node_at` pins the heading at every node so the re-fit reproduces every
untouched record (exact for line/arc/spiral; a paramPoly3 covering record is
re-fitted approximately with the one-time notice).

## Connectivity policy

What happens to links when a topology edit changes the graph:

| Operation | Junction involvement | Road-road links |
|---|---|---|
| **Move** | refused if the road is in / touches a junction (its pose is generated) | links between two moved roads survive; a link leaving the moved set is cleared on **both** sides, in the same command |
| **Split** | (M2) the successor-side junction arm/connection/lane-links remap onto the new tail (#92) | the two halves are linked head↔tail with identity lane links |
| **Merge** | **v1 refuses** any junction involvement (follow-up: junction-aware merge) | the joining ends must be free or link only to each other; `b`'s far-end link and any far neighbor's back-link re-point onto the merged road |

The move's junction refusal and merge's junction refusal both use
`junctions_touching`, which covers the connecting-road, arm, and pred/succ-link
cases.

## Merge preconditions

`check_mergeable(a, b)` (End of `a` → Start of `b`; `reverse_road` is deferred,
so the editor normalizes the argument order) refuses in this order, each with a
verbatim message the UI shows:

1. **valid, distinct ids** — self-merge and stale ids.
2. **junction-free** — neither road participates in a junction (ASAM OpenDRIVE
   §10 road linkage / §12 junctions: a junction road's geometry is owned by the
   junction, not freely editable).
3. **joining ends free** — a's End / b's Start link only to each other, else
   "already connected to another road"; the wrong orientation (End↔End /
   Start↔Start) reports "the roads meet end-to-end — reverse one first (coming
   soon)".
4. **position gap** ≤ `tol::kMergePositionGap` (1 cm) — else the distance is
   reported so the editor can offer to close it (auto-snap is a follow-up).
5. **heading gap** ≤ `tol::kMergeHeading` (1 mrad) — the two ends must be
   tangent-continuous (ASAM OpenDRIVE §7 plan view: contiguous geometry).
6. **seam profile equality** (ASAM OpenDRIVE §9 lanes) — same lane set and
   type, matching width, road mark, and lane offset at the seam, and matching
   elevation z + grade (§8 road elevation). Values must match; sections are
   **concatenated, never coalesced** — so split→merge is geometry-identical, not
   byte-identical (the seam section boundary survives). Section coalescing is a
   follow-up.

The weld re-anchors `b`'s geometry onto `a`'s exact end pose, absorbing the
≤ tolerance residual so the seam is vertex-exact (no duplicated station).

## Context menus

`editor/src/app/context_menu.{hpp,cpp}` is a headless **descriptor builder**
(`build_context_menu(MenuContext, ContextMenuDeps) → vector<MenuItem>`) plus a
thin `assemble_context_menu` that wraps it in a `QMenu`. The builder is the
single source of truth the viewport uses today and the scene tree / guided tour
(#114) will consume, so a right-click means the same thing everywhere. The
matrix per context (road body / node / junction / empty) is unit-tested without
a QMenu. RMB is orbit-or-menu, disambiguated by a movement threshold in the
viewport (not `contextMenuEvent`, whose platform timing conflicts with orbit).

## reverse_road — deferred (with the full flip inventory)

Shipping only End(a)→Start(b) merges keeps the merge honest; `reverse_road` is
closed-form throughout but touches every subsystem, so it is a dedicated
follow-up. Reversing a road must:

- reverse the geometry-record order and, per record, swap start/end and negate
  curvature (spiral `curv_start↔curv_end` negated; arc curvature negated;
  paramPoly3 `u,v` mirrored);
- negate every lane id and mirror the width polynomials to the reversed station;
- swap `SolidBroken ↔ BrokenSolid` road marks and mirror `road_mark` sOffsets;
- mirror + negate superelevation and lane offset;
- flip predecessor/successor and every neighbor's `ContactPoint`;
- reverse object/signal s-coordinates and re-emit the verbatim `*_extras` XML
  correctly — the part that makes an honest v1 refuse rather than silently
  corrupt.

## Follow-ups

- `reverse_road` (unblocks end-to-end merges; inventory above).
- Junction-aware merge and move (remap arms/connections like split's #92).
- Merge section coalescing (byte-identical split→merge).
- Multi-road endpoint snapping on move; merge auto-snap offer.
- Scene-tree right-click reusing the builder; guided-tour coverage (#114).
