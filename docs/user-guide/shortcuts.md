# Keyboard shortcuts

*Every keyboard binding in the RoadMaker editor, in one place.*

Nothing here is only reachable by keyboard: each of these is also a menu or
toolbar entry, and the keys are accelerators for what you can already point at.
Mouse and trackpad navigation — orbiting, panning, zooming, and the modifier
chords — live in [Camera & navigation](camera-navigation.md).

Keys are written in the cross-platform spelling. Where a platform differs, the
**Notes** column says so: `Ctrl` is `⌘ Command` on macOS for the File and Edit
conventions below.

> The tables below are **generated** from the editor's shortcut table
> (`editor/src/app/shortcut_registry.cpp`), which is also what the editor binds
> from. A CI test compares this page against that table, so a binding cannot
> change without this page changing with it. Edit the table, not the tables
> below.

## File

| Action | Shortcut | Notes |
|---|---|---|
| New scene | `Ctrl+N` | ⌘N on macOS |
| Open an OpenDRIVE (.xodr) file | `Ctrl+O` | ⌘O on macOS |
| Save | `Ctrl+S` | ⌘S on macOS |
| Save as… | `Ctrl+Shift+S` | ⇧⌘S on macOS |
| Quit | `Ctrl+Q` | ⌘Q on macOS, where the platform menu owns it |

## Edit

| Action | Shortcut | Notes |
|---|---|---|
| Undo | `Ctrl+Z` | ⌘Z on macOS |
| Redo | `Ctrl+Shift+Z` | Ctrl+Y also works on Windows; ⇧⌘Z on macOS |

## Tools

| Action | Shortcut | Notes |
|---|---|---|
| Select/Move tool | `Q` |  |
| Move tool | `M` |  |
| Create Road tool | `C` |  |
| Edit Nodes tool | `N` |  |
| Lane tool | `L` |  |
| Elevation tool | `E` |  |
| Create Junction tool | `J` |  |
| Split tool | `K` |  |
| Delete tool | `X` |  |
| Lane Add tool | `A` |  |
| Lane Form tool | `Shift+A` |  |
| Lane Carve tool | `Shift+C` |  |
| Crosswalk & Stop Line tool | `W` |  |
| Marking Point tool | `S` |  |
| Marking Curve tool | `Shift+W` |  |
| Prop Point tool | `T` |  |
| Prop Curve tool | `Shift+T` |  |
| Prop Span tool | `Shift+S` |  |
| Prop Polygon tool | `Shift+P` |  |
| Corner tool (junction fillets) | `Shift+R` | plain R is the Prop Polygon tool's re-scatter key |
| Stop Line tool (junction stop lines) | `Shift+O` | F flips the active line while the tool is up |
| Junction Span tool (virtual junctions over a road) | `Shift+J` | plain J is the Create Junction tool |
| Lane Width editor (2D) | `Shift+L` |  |

## View

| Action | Shortcut | Notes |
|---|---|---|
| Frame the selection (the whole scene when nothing is selected) | `F` |  |
| Frame on the point under the cursor (keeps the zoom) | `V` |  |
| Show or hide the active tool's hint in the viewport corner | `H` |  |
| Perspective projection | `P` |  |
| Orthographic projection | `O` |  |
| Look from the north | `Num+8` or `8` | Numpad; the top-row digit is the alternate |
| Look from the south | `Num+2` or `2` | Numpad; the top-row digit is the alternate |
| Look from the west | `Num+4` or `4` | Numpad; the top-row digit is the alternate |
| Look from the east | `Num+6` or `6` | Numpad; the top-row digit is the alternate |
| Top-down view, north up | `Num+5` or `5` | Numpad; the top-row digit is the alternate |

## Help

| Action | Shortcut | Notes |
|---|---|---|
| Open the user guide | `F1` |  |
