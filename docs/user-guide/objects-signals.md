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

The remaining painted and structural families — **crosswalks**, **stop
lines**, **lane arrows**, and **poles** — still author from the Python package
(above) or another OpenDRIVE tool; open the result in the editor to inspect it.
Their in-editor placement follows in the M3a standards track
([#72](https://github.com/Robomous/RoadMaker/issues/72)).

## Reference

- [M3a kernel — objects & signals](../design/m3a/01_kernel_objects_signals.md)
  — the data model, the authored (GS-1) set, and the validation rules.
- [M3a road-mark completions](../design/m3a/02_road_marks.md) — colour,
  multi-line geometry, and object-based crosswalks / stop lines / arrows.
