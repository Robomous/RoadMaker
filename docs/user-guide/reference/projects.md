# Projects

*Group related scenes and shared Library assets in one folder — a project is a
plain directory with a `project.json` manifest, nothing heavier.*

## What a project is

A project is a directory containing:

- `project.json` — the manifest: the project's name, a schema version, and the
  scene you were last working on.
- **Scenes** — ordinary `.xodr` files at the top level of the directory. There
  is no separate scene format or registry: any scene in the folder belongs to
  the project, and every scene still opens standalone outside it.
- `<scene>.rmscene.json` *(written for you)* — one small companion file per
  scene, holding the view you left it at. Never required to open a scene, and
  never part of the OpenDRIVE. See [where things are saved](#where-things-are-saved).
- `assets/` *(optional)* — the project's asset folder, browsable from the
  Library while the project is open (see below). `assets/library/manifest.json`
  inside it is a per-project Library catalogue that overlays the built-in one.

## Steps

1. **File ▸ New Project…** — pick (or create) a folder, then give the project
   a name. The folder becomes the project.
2. **File ▸ New** and save the scene into the project folder — it is now one
   of the project's scenes.
3. Reopen the project any time from **File ▸ Open Project…** or its tile in
   the **Recent projects** section of the welcome screen. It reopens on the
   scene you left, at the camera you left it at. Each tile shows the project's
   name and scene count; clicking it lists the project's scenes (with their
   thumbnails) and offers **New Scene in Project**.

While a project is open, its name shows in the window title, and the Open and
Save dialogs default into the project folder.

Opening any `.xodr` that sits inside a project folder — from the recent list,
a file dialog, or drag-and-drop — automatically opens its project too. Opening
a standalone scene leaves any project.

## Shared Library assets

Give the project an `assets/library/manifest.json` (the same schema as the
built-in Library catalogue) and its items appear in the
[Library](library.md) for **every scene of the project**:

- an item whose `key` matches a built-in item **replaces** it — the project's
  version wins;
- new keys (and new categories) are **added** to the catalogue.

The overlay is removed when the project closes or another project opens.

## Browsing the asset folder

Everything under `assets/` — not just the overlay manifest — is browsable from
the lower half of the [Library](library.md#project-files) dock while the project
is open, as a live folder tree with thumbnails. RoadMaker watches the folder, so
files copied in, renamed, or deleted from the OS file manager appear and
disappear without a restart. Nothing creates `assets/` for you: the Library says
which path to make, and browsing never writes into the project.

## Where things are saved

RoadMaker keeps its own conveniences strictly out of your OpenDRIVE. Three
separate places, in order of how much you can afford to lose:

| File | Holds | If you delete it |
| --- | --- | --- |
| `<scene>.xodr` | the scene: roads, lanes, junctions, signals, props | the scene is gone |
| `<scene>.rmscene.json` | the camera you left the scene at, and its render mode | the scene opens exactly as before, framed to fit |
| `project.json` | the project name and its last-opened scene | the scenes become standalone files |

The `.xodr` is **pure ASAM OpenDRIVE** and stays that way — saving a scene never
puts a byte of editor state into it, so a file exported to another tool carries
nothing RoadMaker-specific it should not. That is why the view lives beside the
scene rather than inside it.

A `.rmscene.json` is optional in both directions: a scene authored elsewhere
opens and edits perfectly well without one, and a stale or hand-broken one is
ignored (RoadMaker frames the scene as usual and rewrites the file on the next
save). Both files are small, readable JSON and are safe to commit to version
control alongside the scene — or to leave out of it, if you would rather each
person keep their own view.

## Notes

- A project is *association*, not a container: deleting `project.json` simply
  makes the scenes standalone again. Nothing about the `.xodr` files changes.
- Scenes are discovered by glob, top level only — subfolders (e.g. `assets/`)
  are never scanned for scenes.
- `project.json` and `<scene>.rmscene.json` are forward-compatible: a newer
  schema version opens best-effort with a warning, and any setting written by a
  newer RoadMaker is preserved when an older one saves over it — so sharing a
  project between versions never quietly discards the other's work.

## Reference

[Library](library.md) for the catalogue the overlay extends, and
[Save & export](save-export.md) for where scenes are written.
