# Projects

*Group related scenes and shared Library assets in one folder — a project is a
plain directory with a `project.json` manifest, nothing heavier.*

## What a project is

A project is a directory containing:

- `project.json` — the manifest: the project's name (and a schema version).
- **Scenes** — ordinary `.xodr` files at the top level of the directory. There
  is no separate scene format or registry: any scene in the folder belongs to
  the project, and every scene still opens standalone outside it.
- `assets/library/manifest.json` *(optional)* — a per-project Library
  catalogue that overlays the built-in one while the project is open (see
  below).

## Steps

1. **File ▸ New Project…** — pick (or create) a folder, then give the project
   a name. The folder becomes the project.
2. **File ▸ New** and save the scene into the project folder — it is now one
   of the project's scenes.
3. Reopen the project any time from **File ▸ Open Project…** or its tile in
   the **Recent projects** section of the welcome screen. Each tile shows the
   project's name and scene count; clicking it lists the project's scenes
   (with their thumbnails) and offers **New Scene in Project**.

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

## Notes

- A project is *association*, not a container: deleting `project.json` simply
  makes the scenes standalone again. Nothing about the `.xodr` files changes.
- Scenes are discovered by glob, top level only — subfolders (e.g. `assets/`)
  are never scanned for scenes.
- `project.json` is forward-compatible: a newer `project_version` opens
  best-effort with a warning, and unknown keys are ignored.

## Reference

[Library](library.md) for the catalogue the overlay extends, and
[Save & export](save-export.md) for where scenes are written.
