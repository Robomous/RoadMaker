# Terrain Brush

*Sculpt the scene's ground by hand — raise hills, carve hollows, and smooth
rough spots — and import a real digital elevation model (DEM) to start from.*

## Before you sculpt: a terrain field

The brush edits the scene's **terrain height field** — the same ground the roads
shape (see [Terrain](terrain.md)). A scene has no field until you create one, so
first choose **Edit ▸ Terrain ▸ Create Terrain Field**. That lays a flat field
over the scene; it looks unchanged until you sculpt it or raise a road through
it. With no field, the Terrain Brush tells you to create one and does nothing.

## Sculpting with the brush

1. Activate the **Terrain Brush** tool (**⇧B**), in the *Terrain & Structures*
   toolbar group.
2. Pick a **mode**, **radius** and **strength** in the options row under the
   toolbar:
   - **Raise** pushes the ground up, **Lower** pushes it down — by *strength*
     metres at the centre of the brush, fading smoothly to nothing at the rim.
   - **Smooth** relaxes the ground toward its local average, flattening bumps
     and ridges without moving flat areas.
   - **Radius** is the brush's reach in metres; **strength** is how hard one
     pass pushes (metres for raise/lower, blend amount for smooth).
3. **Drag** across the ground. The hill or hollow forms live as you move, and
   the surrounding ground meets your edit smoothly. Roads keep their own
   elevation: the ground still blends up to each kerb within a skirt band, so
   sculpting near a road never leaves a cliff at its edge.
4. **Release** to finish. The whole stroke — however long you dragged — is a
   single undo step.

Repeat passes build up: drag over the same spot again to raise it further, or
switch to Smooth to settle it down.

## Importing a DEM

To start from real-world elevation instead of a flat field, choose
**Edit ▸ Terrain ▸ Import DEM…** and pick an **ESRI ASCII grid** (`.asc`) file.
RoadMaker reads the grid and installs it as the scene's terrain field, placed at
the coordinates the file declares. The roads then conform to the imported ground,
and you can refine it with the brush.

Only the dependency-free ESRI ASCII format is supported here; other raster
formats (GeoTIFF and friends) arrive with the wider GIS import work. A grid cell
with no data is read as height zero, with a warning rather than a hole.

## Persistence

The field — sculpted or imported — is saved as a `.asc` sidecar next to the
`.xodr` and reloads with the scene, exactly as a created field does (see
[Terrain](terrain.md)).

## See also

- [Terrain](terrain.md) — how the height field couples to road elevation.
- [Ground surfaces](ground-surfaces.md) — the road-enclosed ground patches that
  sit on top of the field.
