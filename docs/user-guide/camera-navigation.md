# Camera & navigation

*Moving the viewport camera: what orbits, what zooms, what pans, and where the
pivot lives.*

RoadMaker's camera is an **orbit camera**. It always looks at a single point —
the **pivot** — and every navigation gesture is defined relative to it: you
orbit around it, zoom toward it, and pan it along with the camera. In an empty
scene the pivot starts 1.5 m above the world origin, roughly eye height, so
there is always something in the world to turn around.

Bindings are written macOS-first. `⌥` is **Option** on macOS and **Alt**
elsewhere; the two names mean the same key throughout this page.

## The bindings

| Gesture | Does |
|---|---|
| `⌥` + left-drag | **Orbit** around the pivot — the pivot stays put on screen |
| `⌥` + right-drag | **Zoom** toward or away from the pivot; drag **up to zoom in** |
| Wheel | **Zoom** toward the pivot |
| `⌥` + left-drag + right-drag | **Pan** — camera and pivot move together, 1:1 with the ground under the cursor |
| Middle-drag | **Pan** (the same thing, without `⌥`) |
| `⌥` + `⇧` + left-drag + right-drag | **Raise or lower the pivot** — drag up to raise it |
| Right-drag | **Orbit** (see [legacy alternate](#the-legacy-right-drag-orbit)) |
| Right-click | Open the [context menu](context-menus.md) for whatever is under the cursor |
| `F` | **Frame** the selection (or everything, with nothing selected) |
| `V` | **Frame on the cursor** — move the pivot to the point under it |
| `O` / `P` | **Orthographic** / **perspective** projection |
| Numpad `8` `2` `4` `6` | Look from **north / south / west / east** |
| Numpad `5` | **Top-down**, north up |

Without a numeric keypad, the **top-row digits** `8` `2` `4` `6` `5` do the
same. Every one of these also lives in the **View** menu — the keys are
shortcuts, not the only way in.

A two-button chord is exactly what it sounds like: hold `⌥`, press and hold the
left button, then add the right button without letting go. Both buttons stay
down for the whole gesture.

## How the chords behave

**The gesture is decided when you press the buttons, not while you drag.** Once
a drag is underway it keeps doing what it started doing, so letting go of `⌥`
halfway through an orbit finishes the orbit rather than dropping it. The other
side of that bargain: adding `⇧` to a pan already in progress does **not** turn
it into a pivot lift — release and press the chord again for that.

Releasing one button of a two-button chord falls back to what the remaining
button means. Lifting the right button out of an `⌥` pan leaves you holding
`⌥` + left, so the gesture becomes an orbit and keeps going.

**Navigation always wins.** While a chord is live it owns the mouse: the active
tool and the transform gizmo never see those events, so you cannot nudge a road
while trying to frame it. The one exception is a gizmo drag already in flight —
that keeps the mouse until you release it.

## Panning is anchored to the ground

A pan grabs the ground point under the cursor and keeps it there: the scene
tracks your cursor at 1:1, like dragging a paper map. Zoom level doesn't
change the feel — panning far out moves you proportionally further per pixel.

When the camera is nearly level with the horizon there may be no ground point
to grab (the ray escapes to the sky). The pan then falls back to sliding the
view at the correct scale for the current distance, so it still moves the
expected amount; it just can't pin a specific point.

## The pivot

Everything orbits and zooms around the pivot, so **moving the pivot is how you
choose what to look at**.

- `⌥` + `⇧` + left-drag + right-drag lifts it vertically. Drag up to raise it,
  down to lower it — useful for orbiting an overpass or a signal head instead
  of the road deck below it.
- Raising the pivot never changes how panning or zooming feel; those follow the
  orbit distance, not the pivot's height.
- Panning moves the pivot horizontally with the camera and leaves its height
  alone.

Zooming *in* moves the pivot too, once you go far enough: rather than stopping
dead when you reach it, the camera **pushes past** — the pivot slides forward
and you keep travelling at the same rate, so you can fly through a scene with
the wheel alone. Zooming back out never drags the pivot with it.

## Framing

Framing is how you point the camera at something without flying there by hand.

- **`F`** frames the selection. It frames *exactly* what you selected: a signal
  frames the signal, a lane frames that lane, a junction frames its floor — not
  the road each of them happens to sit on.
- **`F` with nothing selected** frames all the content in the scene, keeping
  your current viewing angle.
- **`F` in an empty scene** returns to the starting pivot above the origin.
- **`V`** frames on the point under the cursor: the pivot jumps there and the
  camera keeps both its angle and its zoom distance. It's the fast way to say
  "orbit around *that* instead" — hover, tap `V`, and keep working.

## Projection

`P` is perspective (the default): near things look bigger, and the view has
depth. `O` is orthographic: parallel projection with no foreshortening, so
parallel lines stay parallel and distances read true across the frame — useful
for checking alignment in plan view.

The two share their framing, so **toggling doesn't jump**: whatever fills your
screen in one mode fills it in the other. In orthographic mode the wheel zooms
toward the cursor rather than the screen centre.

## Cardinal views

The numpad digits snap the view to a compass direction: `8` north, `2` south,
`4` west, `6` east, and `5` straight down with north up.

They snap the **direction only** — your pivot, zoom, and (except for top-down)
your pitch stay exactly as they were. Pressing `4` re-angles the view onto what
you were already looking at; it doesn't send you somewhere else.

## The legacy right-drag orbit

Right-drag orbits **without** holding `⌥`. This is the binding RoadMaker had
before the orbit-pivot model and it is kept as an alternate, mostly for mice
without a usable middle button.

It shares the right button with the context menu, so the two are told apart by
distance: a right-press that moves less than about 4 pixels before you let go
opens the context menu, and anything further starts orbiting and suppresses the
menu. A drag that has begun orbiting never pops the menu, however far back
toward the press point you drag.

Prefer `⌥` + left-drag when you have the choice — it can't be mistaken for a
click.

## Platform notes

- **macOS** — `⌥` is Option. All chords work on a trackpad, but the two-button
  chords need a real mouse (or a trackpad configured for right-click); the
  middle-drag pan needs a three-button mouse.
- **Linux** — many desktop environments grab `Alt` + drag to move the window
  itself. If `⌥` + left-drag moves the RoadMaker window instead of orbiting,
  either rebind that in your window manager (commonly to the Super key) or use
  the alternates: right-drag to orbit and middle-drag to pan.
- **Windows** — no caveats; Alt is Alt.

## See also

- [Viewport hints](viewport-hints.md) — the corner hint card, and how to hide it
- [Right-click menus](context-menus.md) — the menu the short right-click opens
- [Moving and transforming](moving-and-transforming.md) — moving *content*,
  as opposed to moving the camera
