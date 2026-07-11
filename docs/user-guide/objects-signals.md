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

In-editor object and signal **placement** — a properties-panel workflow plus a
read-only library panel with drag-to-place — arrives with its editor features
later in milestone M3a. Until then, author objects and signals from the Python
package (above) or another OpenDRIVE tool and open the result in the editor to
inspect it.

## Reference

- [M3a kernel — objects & signals](../design/m3a/01_kernel_objects_signals.md)
  — the data model, the authored (GS-1) set, and the validation rules.
- [M3a road-mark completions](../design/m3a/02_road_marks.md) — colour,
  multi-line geometry, and object-based crosswalks / stop lines / arrows.
