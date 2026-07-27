# Persistence layers

*Where each datum lives, and why: pure ASAM first, RoadMaker enrichment layered
on top without ever breaking interchange. The decision is
[ADR-0008](../decisions/0008-persistence-layers-asam-first.md); this page is the
implementer's map of it.*

## The three layers

| Layer | Lives in | Holds | Lost if deleted |
|---|---|---|---|
| **0 — pure ASAM** | `<scene>.xodr` | roads, lanes, junctions, signals, objects — everything the standard can express | the scene |
| **1 — ASAM-adjacent** | `<userData code="rm:…">` *inside* the `.xodr` | RoadMaker annotations on ASAM entities (`rm:stopline`, `rm:phases`, `rm:maneuver`, …) | authoring detail; the scene still loads everywhere |
| **2 — native container** | `project.json` + `<scene>.rmscene.json`, *beside* the `.xodr` | editor and session state with no business in an ASAM file | comfort only — never content |

Layer 0 is inviolable: an exported `.xodr` is always valid, self-contained, and
consumable by a tool with zero RoadMaker knowledge. Layer 1 rides inside it
through the standard's own extension mechanism and is ignored by other readers.
Layer 2 never touches it at all.

The registry of `rm:` codes and their payload grammars is in ADR-0008; each
owning sprint defines its own. `fmt-s2` (#326) adds the conformance tests.

## Layer 2 — the container

Deliberately a **directory**, not a single-file archive: it is git-friendly,
diffable, partial-write-safe, and keeps every `.xodr` standalone-openable.

### `project.json` (v2)

Marks a directory as a project. Written by `Project::create()` and rewritten by
`Project::save()`; see [`editor/src/document/project.hpp`](../../editor/src/document/project.hpp).

```json
{
  "project_version": 2,
  "name": "Downtown",
  "last_scene": "main.xodr"
}
```

- `last_scene` is **project-relative** with `/` separators. A path outside the
  project clears the field rather than storing a `../` escape, and a scene that
  no longer exists resolves to nothing — reopening the project just lands on the
  welcome view.
- `name` is omitted when it was only inferred from the directory name, so a
  minimal manifest stays minimal.

### `<scene>.rmscene.json` (v1)

One per scene, beside its `.xodr`, named from the scene's **stem** (`town.xodr`
→ `town.rmscene.json`), matching how the kernel names its terrain sidecar. See
[`editor/src/document/scene_sidecar.hpp`](../../editor/src/document/scene_sidecar.hpp).

```json
{
  "scene_version": 1,
  "view": {
    "target": [12.5, -3.25, 1.5],
    "yaw": 0.8,
    "pitch": 0.9,
    "distance": 80,
    "projection": "perspective"
  },
  "textured": true
}
```

- `view` is the camera pose in the **kernel frame** (right-handed, Z-up, meters
  and radians) — exactly what `OrbitCamera::set_pose()` restores. `projection`
  is `perspective` or `orthographic`.
- `textured` is the per-scene render mode. It overrides the application default
  for this scene only and is never written back to `QSettings`: a per-scene
  override is not a preference.
- Reserved and round-tripped untouched when present: `snap` (snapping settings —
  the tools have the plumbing but no UI yet) and `session`. ADR-0008 also lists
  prop-set definitions, material-overlay references and workspace extents as
  future Layer-2 residents; they land in their owning sprints without a schema
  break, because of the retention rule below.

## The rules that make it safe

**Never a byte into the `.xodr`.** Layer 2 is a separate file, so the ASAM
output is identical with and without it. There is no separate "export ASAM"
action — `Document::save()` *is* the export — which is why Layer-0 purity is
asserted directly (`test_scene_state.cpp`).

**Degradation is one-way and free.** A missing sidecar (every plain `.xodr`) is
silent. A malformed one warns and is replaced on the next save. Neither can
block a load or cost scene content. "Stale" means *missing or unparseable* and
nothing else — a camera parked far from the geometry is legitimate authoring, so
there is no geometric staleness check and no content hash.

**All-or-nothing within a block.** A `view` missing a field, carrying a
non-finite number, or naming an unknown projection is dropped whole rather than
restored half-applied — the same rule the `rm:` carriers follow.

**Unknown keys survive a rewrite.** Both files keep the parsed root and *merge*
the fields they own over it. A setting written by a newer RoadMaker is preserved
when an older one saves, and the version is re-emitted as parsed, never
downgraded. This is the opposite of `LibraryManifest`'s verbatim-wins rule,
where the raw block is authoritative — here the modeled fields are the live
values being saved and `raw` is only the forward-compat carrier.

**Floats print short and reload exact.** Camera state is `float`; JSON is
`double`. The writer emits the shortest decimal that still reloads the identical
float (capped at `FLT_DECIMAL_DIG` = 9 significant digits), so the file reads
`0.8` rather than `0.800000011920929`, reloads bit-for-bit, and is a fixed point
of write → parse → write — which is what makes save → load → save
byte-identical.

## Who writes what

- **`Document::load()`** reads the sidecar after the network is in place, then
  emits `scene_state_loaded()` **last** — `loaded()` arms the viewport's
  post-load auto-framing, and a restored camera has to overrule it.
- **`Document::save()`** writes the sidecar atomically (`QSaveFile`) after the
  `.xodr` lands. A failure is logged, never fatal.
- **The state provider** is how `Document` — which is QtCore-only and cannot see
  the viewport — obtains the live camera. `MainWindow` installs a callback that
  takes the state **by reference**, seeded from what was loaded, so it overwrites
  only the fields it owns and cannot drop retained keys.
- **`AutosaveManager`** writes the same state beside each recovery copy, through
  the same `current_scene_state()`, so a recovered document never comes back at
  a different camera than Save would have recorded.
- **`Document::mark_recovered()`** re-seats the state from the *original*
  scene's sidecar when the recovery copy had none — otherwise the next Save
  would overwrite the user's real sidecar with defaults.

## Reference

- [ADR-0008 — persistence layers, ASAM first](../decisions/0008-persistence-layers-asam-first.md)
- [Editor architecture](editor.md) · [Kernel architecture](kernel.md)
- [OpenDRIVE domain notes](../domain/opendrive.md)
- User-facing: [Projects](../user-guide/projects.md)
