# Objects & signals

*Add the scene furniture that makes a road network legible and simulator-ready:
crosswalks, props, lane arrows, stop lines, traffic lights, and signs.*

RoadMaker follows the ASAM OpenDRIVE split:

- **Objects** (`<object>`) — crosswalks, trees and other props, poles, and
  painted road markings that are not mandatory control signals (lane arrows,
  stop lines, zebra crossings).
- **Signals** (`<signal>`) — traffic-control signs and lights: traffic lights,
  speed limits, pedestrian-crossing warnings.

Each is placed in road `s`/`t`/`zOffset` coordinates and validated against the
relevant ASAM rules, so an exported `.xodr` carries real semantics — not just
geometry.

## Authoring today (kernel & Python)

The kernel represents, validates, writes, and meshes the full GS-1 object and
signal set, and you can author it from the Python package now:

| Example | What it authors |
|---|---|
| [`place_objects.py`](../../python/examples/place_objects.py) | A crosswalk, a pole, and a tree line (`<repeat>`) |
| [`place_signals.py`](../../python/examples/place_signals.py) | A traffic light, a speed-limit sign, a pedestrian-crossing sign |
| [`road_marks.py`](../../python/examples/road_marks.py) | A true double-yellow centre line and dashed white edge lines |

```sh
python python/examples/place_signals.py signals.xodr
```

Painted markings that are objects — **crosswalks** (zebra), **stop lines**, and
**lane arrows** (left / straight / right) — mesh as generated paint geometry
(no asset needed), and multi-line lane marks such as `solid solid` render as
true dual stripes.

## In the editor

**Props and signals** place directly in the editor: drag one from the
[**Library**](library.md) onto a road and RoadMaker adds it, snapped to the
nearest road in `s`/`t`:

- **Props** (trees, shrub) become an `<object type="tree">`.
- **Traffic lights** become a dynamic `<signal>`; **traffic signs** a static
  one.

![Trees placed along a road, with the Library catalogue open](img/library.png)

Placed props and signals select, move, delete, duplicate, and round-trip
through save/reload and the glTF / USD exports. Select a signal and its
properties panel shows the kind (dynamic light or static sign), its
type / subtype and country, and lets you nudge its road-relative pose —
`s` along the road, `t` across it, and the heading offset — each edit an
undoable command.

### Junction markings

The painted markings a junction needs on every arm author in one action each.
Right-click the **junction floor** — the blended surface in the middle, between
the arms — and pick from:

| Action | Authors |
|---|---|
| **Add crosswalks to all arms** | one zebra `<object type="crosswalk">` per arm, spanning its driving lanes just inside the junction |
| **Add stop lines to all arms** | a solid `<object type="roadMark" subtype="signalLines">` across each arm's approach lanes, set back behind the crosswalk |
| **Add lane arrows to all arms** | a straight arrow on each approach lane, pointing into the junction and set back behind the stop line |
| **Add centre lines to all arms** | a double-yellow centre line (`roadMark` `solid solid`, yellow) on lane 0 of each arm, replacing the one the road template laid down |

Each action covers **every arm at once and is a single undo step** — Ctrl+Z
takes the whole batch back. An action is greyed out when the junction has no
arms it can resolve (a junction read from another tool's file, say), so it
never silently does nothing.

The two-way arms of a plain crossing get a stop line and an arrow **per
approach** — one for each direction of travel — since each side approaches the
junction on its own lanes.

> **Turn arrows.** The editor authors the straight glyph. The `arrowLeft` and
> `arrowRight` variants are modelled and render, but choosing which lane gets
> which is Python-side for now — `edit.junction_lane_arrows(network, junction,
> glyph)` takes a callable that picks the subtype per approach lane.

**Poles** still author from the Python package (above) or another OpenDRIVE
tool; open the result in the editor to inspect it.

## Reference

- [M3a kernel — objects & signals](../design/m3a/01_kernel_objects_signals.md)
  — the data model, the authored (GS-1) set, and the validation rules.
- [M3a road-mark completions](../design/m3a/02_road_marks.md) — colour,
  multi-line geometry, and object-based crosswalks / stop lines / arrows.
