# Editing attributes

*The Properties panel is where you read and change the fields of whatever is
selected. Two gestures make it faster than typing: scrubbing a number, and
dropping an asset into a slot.*

Select something in the viewport or the scene tree and the panel shows its
fields. Every change is an undoable command, so anything on this page is
reversible with **Undo**.

## Scrub a number by dragging its name

Numeric attributes can be dragged instead of typed. **Drag the attribute's
name** — not its box — left or right, and the value follows your cursor while
the scene updates live.

| While dragging | Does |
|---|---|
| Drag right / left | Increase / decrease the value |
| Hold `⇧` | Fine — a tenth of the normal rate |
| Hold `⌘` (macOS) / `Ctrl` | Coarse — ten times the normal rate |
| `Esc` | Cancel: the value snaps back to where the drag started |

The cursor turns into a horizontal resize arrow over any name you can scrub.
A plain click does nothing, so you can't nudge a value by accident.

**A whole drag is one edit.** However far you drag, and however many times the
scene redraws on the way, one Undo takes you back to the value you started
from — you never have to undo a drag frame by frame. A drag that ends back
where it began isn't an edit at all and leaves the undo history untouched.

Modifiers apply to the motion you make *while holding them*, so tapping `⇧`
partway through refines from wherever the value has already reached rather than
jumping somewhere new.

Scrubbing is available on lane **Width**, road-mark **Mark width**, elevation
**Height**, a prop's **s**, **t**, **Heading**, **Z offset** and **Height**, and
a signal's **s**, **t**, **Heading offset** (which, once you touch it, becomes a
hand-set override that only the signal's **Auto facing** button undoes) and
**Mounting height**. Each has its
own rate, chosen so a comfortable drag covers a useful range — roughly 2 m of
lane width, or 10 m along a road, per screen-width drag. Scrubbing respects the
same limits as typing: it cannot push a value out of the attribute's range.

### Editing several things at once

A prop's **Height** applies to the *whole selection*, not just the primary
object: select any number of props and one drag resizes them all by the same
factor — relative sizes preserved — while typing a height makes them all exactly
that tall. It is still one undo step. See
[Resizing props](objects-signals.md#resizing-props).

## Where a thing stands

Props and signals are placed *along a road*, so their position is two numbers in
the road's own frame — **s**, the distance travelled along it, and **t**, the
sideways offset (negative to the right of the reference line, positive to the
left) — plus **Z offset** / **Mounting height** above the road surface and a
**Heading** about the vertical axis. All of them are editable, and all of them
are what the gizmo writes when you drag it, so typing and dragging are the same
edit by two routes.

Underneath sits a read-only **World** row: where the thing actually ends up in
world coordinates once the road's curve and elevation profile are applied. It is
read from the same data the viewport draws, so it can never disagree with what
you see.

A marking — a crosswalk, a stencil, a marking curve — shows its position but
does not let you type it. Its shape is stored as an outline in road coordinates,
and moving only the origin would leave the two out of step. Move markings in the
viewport instead.

## Lane direction and mark colour

A selected lane also carries a **Direction** — which way traffic runs in it,
relative to the road's own direction — and its boundary line carries a **Mark
colour**. Road templates set both (North American practice: yellow for the
centre line, white for lane lines), and you can change either afterwards. The
Direction control is disabled for the centre lane, which has no travel of its
own.

## Slots: drop an asset in

Some attributes point at an asset rather than holding a number. Those appear as
a **slot**: a framed box showing what the attribute currently references.

- **Drop** an item from the [Library](library.md) onto the slot to point the
  attribute at it. The slot highlights while a droppable item is over it.
- **Click** the slot to jump to the matching Library category, so you can find
  a replacement without hunting for it.

Only matching items are accepted; anything else declines the drop and leaves
the attribute alone.

### The prop Model slot

Selecting a placed prop shows a **Prop** section with a **Model** slot naming
the model it renders. Drop a different prop from the Library on it and the prop
becomes that model — its bounding size follows, so its declared volume always
describes what it actually is. Its position and heading don't move: the slot
changes *what the prop is*, not *where it is*.

Dropping something that isn't a prop model — a road template, say — is refused,
and nothing lands in the undo history.

### Lane material

Selecting a lane shows a **Material** slot in the **Lane profile** section. Drop
a material — **Asphalt**, **Asphalt (worn)**, or **Concrete** — on it, or click
it to jump to the Materials category, and the whole lane takes that surface
material (texture in [textured mode](textured-rendering.md), friction in the
file). The slot is disabled for the centre lane, which carries no material by
rule. You can also drag a material straight onto the lane in the viewport. See
[Materials](materials.md) for what gets stored and how it round-trips.

### Ground surface material

Selecting a [ground surface](scene-tree.md) (an area enclosed by roads) shows a
**Ground surface** section with a **Materials** slot. Drop **Asphalt**,
**Asphalt (worn)**, or **Concrete** from the Library on it — or click the slot
to jump to the Materials category — and the surface re-textures to that paved
look; the slot reflects the current material and clears back to the default
grass on **Undo**. Dropping something that isn't a material is refused with no
undo entry.

The material is stored on the surface and round-trips through save/reload. One
caveat: a surface's identity follows the roads that enclose it, so reshaping the
bounding ring re-derives the surface and drops its material (the same lifecycle
as the surface itself) — re-apply the material after a big topology change.

## See also

- [Library](library.md) — the catalogue slots draw from
- [Objects & signals](objects-signals.md) — placing the props and signals whose
  attributes this page edits
- [Camera & navigation](camera-navigation.md) — moving the view while you edit
