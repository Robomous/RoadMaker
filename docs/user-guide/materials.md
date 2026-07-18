# Materials

*Give a lane surface a look and a friction value — new asphalt, worn asphalt,
or concrete — that renders under light and saves into the `.xodr`.*

A **material** describes what a surface is made of: its texture (colour, surface
normal, roughness) for the [textured render mode](textured-rendering.md), and
its **friction** coefficient, stored in the file for downstream simulation.
RoadMaker ships three: **Asphalt**, **Asphalt (worn)**, and **Concrete**.

Materials are project assets — the textures live in the app, not in your scene.
The `.xodr` records only the *assignment* (which lane got which material),
following the OpenDRIVE `<material>` element ([§11.8.2](../domain/opendrive.md)).

## Applying a material to a lane

Two ways, both one undoable edit:

1. **Drag from the Library.** Open the [Library](library.md), find a material
   under **Materials**, and drag it onto a lane in the viewport. The ghost
   follows the cursor; drop it on the lane surface you want paved. Dropping on
   the centre line or off the road is refused with a hint — a material paints a
   *lane*, and the centre lane carries none by rule.
2. **The lane Material slot.** Select a lane; the **Lane profile** section shows
   a **Material** slot. Drop a material on it, or click it to jump to the
   Materials category in the Library. The slot is disabled for the centre lane.

Either way the whole lane takes that material at friction from the catalogue.
**Undo** clears it. Save and reload keeps it.

## What gets stored

A lane assignment writes a standard `<material>` record on the lane:

```xml
<lane id="-1" type="driving">
  <width sOffset="0" a="3.5" b="0" c="0" d="0"/>
  <material sOffset="0" friction="0.72" surface="rm:asphalt_worn"/>
</lane>
```

- **`surface`** is the material id, namespaced `rm:` so it's recognisable as
  RoadMaker's in a file another tool opens.
- **`friction`** is the catalogue's nominal coefficient for that material —
  written faithfully, never simulated (RoadMaker has no tyre model).

A file authored elsewhere that already carries `<material>` records — including
attributes RoadMaker doesn't model — round-trips **byte-for-byte**: the parser
promotes what it knows and preserves the rest verbatim, so nothing is dropped.

## Where materials show

In [textured render mode](textured-rendering.md), an assigned lane samples its
albedo, normal, and roughness maps under the sun — new asphalt reads glossier
than worn. In **Sober** mode, materials are ignored and every surface draws its
flat colour. A lane with no material assigned looks exactly as it did before —
its lane-type colour or the default asphalt texture.

## See also

- [Library](library.md) — the catalogue you drag materials from
- [Attributes](attributes.md) — the Material slots on lanes and ground surfaces
- [Textured rendering](textured-rendering.md) — the lit mode materials show in
- [Lane profile](lane-profile.md) — the lane attributes materials sit beside
