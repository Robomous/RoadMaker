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
**Height**, and a signal's **s**, **t**, and **Heading offset**. Each has its
own rate, chosen so a comfortable drag covers a useful range — roughly 2 m of
lane width, or 10 m along a road, per screen-width drag. Scrubbing respects the
same limits as typing: it cannot push a value out of the attribute's range.

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

### Ground surface material

Selecting a [ground surface](scene-tree.md) (an area enclosed by roads) shows a
**Ground surface** section with a **Materials** slot. Drop **Asphalt** or
**Concrete** from the Library on it — or click the slot to jump to the Materials
category — and the surface re-textures to that paved look; the slot reflects the
current material and clears back to the default grass on **Undo**. Dropping
something that isn't a material is refused with no undo entry.

The material is stored on the surface and round-trips through save/reload. One
caveat: a surface's identity follows the roads that enclose it, so reshaping the
bounding ring re-derives the surface and drops its material (the same lifecycle
as the surface itself) — re-apply the material after a big topology change.

## See also

- [Library](library.md) — the catalogue slots draw from
- [Objects & signals](objects-signals.md) — placing the props and signals whose
  attributes this page edits
- [Camera & navigation](camera-navigation.md) — moving the view while you edit
