# Textured rendering

*Switch the viewport between the flat **Sober** look and the **Textured**
daytime look — what each is for, and why Sober is the default.*

RoadMaker draws the same network two ways. Neither changes your data: this is a
view setting, not an edit, so it is not on the undo stack and it never touches
the `.xodr`.

## Turning it on

**View ▸ Textured Rendering** — a checkbox. The choice persists across
restarts (it is stored with your editor settings), so set it once and forget
it. There is no keyboard shortcut.

The editor starts in **Sober** mode.

## The two modes

| | Sober (default) | Textured |
|---|---|---|
| Surfaces | Flat colour per lane type | Asphalt and concrete textures |
| Lighting | Even, unlit-looking | Daytime sun + sky, shaded by surface angle |
| Ground | Reference grid | Procedural grass |
| Sky | Flat backdrop | Gradient sky |
| Best for | Authoring and review | Presenting and screenshots |

**Sober** is the default because it is the better *working* view. Flat colour
makes lane types instantly distinguishable, the grid gives you a spatial ruler,
and nothing is shaded darker just because it happens to face away from the sun.
When you are checking that a lane is the type you meant, or that a junction
connected the way you expected, Sober tells you faster.

**Textured** is the presentation view. It is what the golden-scene renders and
release screenshots use — see
[GS-1 "Urban intersection"](../roadmap/golden_scenes/gs1_urban_intersection.md),
which is rendered with this mode on.

## What "textured" does not mean

The lighting is deliberately simple — a single directional sun plus a hemisphere
ambient term. There are **no shadows** and no image-based lighting. Surfaces get
a base colour texture; normal and roughness maps, assignable materials, and
material variants are the next milestone's work
(Materials & Structures, v0.7.0), not something you can configure today.

Markings (crosswalks, stop lines, arrows, lane lines) are drawn unlit in both
modes on purpose, so paint stays legible instead of dimming on shaded arms.

## From the command line

Screenshot mode takes the same switch, which is how CI renders the golden
scenes:

```sh
python scripts/editor_screenshot.py \
  assets/samples/gs1_urban_intersection.xodr out.png \
  --camera gs1 --textured --size 1920x1080
```

Without `--textured` you get the Sober look.

## See also

- [Objects & signals](objects-signals.md) — the props and signals that the
  textured view shows off
- [Save & export](save-export.md) — exported meshes carry their own materials
  and are unaffected by this setting
