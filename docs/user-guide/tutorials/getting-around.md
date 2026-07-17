# Getting around

*Move the camera with confidence: orbit, zoom, pan, frame your selection, and
snap to fixed views. A few minutes here pays off in every other tutorial.*

![The editor viewport framed on a road network from an orbit
camera](img/getting-around.png)

## Before you start

Open any scene with something in it — a sample file, or the network from
[Your first road network](first-road-network.md). You need geometry on screen to
navigate around.

## 1. Orbit, zoom, and pan

The viewport is right-handed and Z-up, and the camera turns around a pivot in the
scene:

- **Orbit** — drag to rotate the view around the pivot.
- **Zoom** — scroll the wheel to move toward or away from the pivot.
- **Pan** — drag to slide the pivot across the ground plane.

The full mouse and modifier map is on the
[Camera & navigation](../camera-navigation.md) page.

## 2. Frame what matters

You rarely want to fly the camera by hand. Instead:

- Select a road (in the viewport or the [Scene tree](../scene-tree.md)) and press
  **Frame Selection** (<kbd>F</kbd>) to fit it to the view.
- Press **Frame on Cursor** (<kbd>V</kbd>) to recentre the pivot under the cursor
  without changing the framing.

Framing moves the pivot, so orbit and zoom afterwards revolve around exactly what
you are working on.

## 3. Snap to fixed views

For precise work, switch to a fixed camera: **Top**, the cardinal directions
(North / South / East / West), and **Perspective** vs **Orthographic** all have
menu entries and shortcuts (the numpad digits, with numpad-less alternates). The
[keyboard shortcuts](../shortcuts.md) page lists every binding, generated from the
editor's own table.

## 4. Make it a habit

Select → frame → orbit is the loop you will use constantly: pick the object, fit
it, then turn the camera to see the detail. Every tool in the other tutorials
assumes you can get the object you care about in front of you quickly.

## Where to go next

- [Your first road network](first-road-network.md) — put the camera to work
  drawing your first roads.
