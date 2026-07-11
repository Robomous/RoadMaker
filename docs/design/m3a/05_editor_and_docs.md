# M3a editor UX, autosave, CI gate, and docs

Covers the non-render editor work: the read-only library panel and object/signal
placement, autosave & crash recovery, the esmini round-trip CI gate, and the
minimal user guide. Editor rules unchanged (CLAUDE.md / qt-editor skill):
widgets stay thin, all mutation flows through `Document` as an `edit::Command`,
all selection through `SelectionModel`, every item model ships its
`QAbstractItemModelTester` gtest.

## 1. Minimal read-only library panel (decision 2)

A new dock panel `editor/src/panels/library_panel.{hpp,cpp}` — **flat list +
drag-to-place, no search, no categories** (the M4 browser subsumes it).

**Issue boundary (clarified 2026-07-11):** this section is
[#50](https://github.com/Robomous/RoadMaker/issues/50) — the `LibraryManifest`
loader's *view* side: `LibraryListModel`, the dock panel, and the **drag
source** (the outgoing MIME payload carrying the library `key`). The viewport
**drop target**, the placement commands it issues, and the properties-panel
sections are §2 = [#72](https://github.com/Robomous/RoadMaker/issues/72).
#50 depends on [#70](https://github.com/Robomous/RoadMaker/issues/70)
(`assets/library.json` + the headless loader ship there); #72 depends on #50.
If #50 lands first, its drag payload is testable without a drop target (model
+ MIME tests only).

- **Model:** `LibraryListModel` (`editor/src/document/`) over the
  `LibraryManifest` loaded from `assets/library.json`
  ([`03`](03_assets.md) §4). A flat `QAbstractListModel`: icon (class glyph) +
  `label`, `key` in a user role. Ships with its `QAbstractItemModelTester`
  gtest (standing rule). Read-only: no add/remove/edit.
- **View:** a `QListView` in a dock, grouped visually by `class` via a section
  header delegate (still one flat model — no tree, no filtering).
- **Interaction:** drag an item → drop on the viewport. The drop carries the
  library `key`; the viewport hit-tests the drop point to a road `s`/`t`
  (reusing the M2 snapping queries in `core/.../edit/snap.hpp`) and issues the
  placement command (§2). No kernel calls from the widget except through
  `Document`.

## 2. Object & signal placement

Two entry points, both producing the same `edit::Command`s from
[`01`](01_kernel_objects_signals.md) §2.4 (`AddObject`/`AddSignal` etc.).
**The kernel command layer itself lands here** (#72): phases 0/1 shipped the
arena + `restore`/`erase_exact` API only, so this issue adds
`edit/operations.hpp` commands for objects *and* signals, their Python
bindings + example (same-PR rule), and the headless apply→revert tests —
see `01` §2.4 phase ownership. Marking placement uses the normative subtypes
of [`02`](02_road_marks.md) §3 (`zebra`, `signalLines`, `arrowLeft`/
`arrowStraight`/`arrowRight`):

- **Library drag-to-place** (§1): drop resolves `key` → manifest item → a new
  `<object>` or `<signal>` at the snapped `s`/`t`, with the manifest's placement
  defaults (`z_offset`, `orientation`, lane-type snap). One command on drop.
- **Properties-panel workflow:** the existing `properties_panel` gains
  object/signal sections when an object/signal is selected — edit `s`/`t`/
  `zOffset`/`hdg`, signal `type`/`subtype`/`value`/`unit`, object `type`/
  `subtype`. Each field commits a `Move*`/`Set*` command (drags = preview +
  one command on release, per the M2 rule).
- **Crosswalks / stop lines / arrows** ([`02`](02_road_marks.md) §3) are placed
  as object markings from a small "road markings" section of the toolbar, with
  lane-snapped defaults (crosswalk spans the arm; stop line spans approach
  lanes; arrow at lane-center with left/straight/right variants).
- **Selection & scene tree:** objects/signals appear in the `SceneTreeModel`
  under their owning road; selection flows through `SelectionModel`; delete uses
  the existing Delete tool (now cascading object/signal leaves).

Everything is headless-testable: placement/move/delete logic lives in commands
and the document, exercised without a window (offscreen), matching the M2 tool
tests.

## 3. Autosave & crash recovery

Editor polish; design decided here (the seed left it to the planning task).

- **Autosave:** `Document` writes a recovery copy of the current network to a
  well-known per-session path (`QStandardPaths::AppDataLocation/recovery/
  <session>.xodr`) on a debounced timer (e.g. 30 s) **and** after every N
  committed commands, whichever comes first. It writes through the same
  version-explicit `write_xodr` as Save — the recovery file is a valid `.xodr`,
  not a bespoke format (the `.xodr`-is-the-project-file rule from M2). A sidecar
  `<session>.json` records the original document path + dirty state + a
  monotonically increasing save token.
- **Crash recovery:** on startup, if a recovery file exists whose sidecar marks
  it newer than the last clean save, prompt: *"Recover unsaved work from
  <time>?"* Recover → load the recovery `.xodr` and mark the document dirty
  against its original path. Discard → delete the recovery set.
- **Cleanup:** a clean Save or explicit close deletes the recovery set;
  autosave never touches the user's actual file (no silent overwrite —
  outward-facing-action caution).
- **Testable headless:** the autosave writer + recovery-decision logic live in
  the document layer (no timer/UI dependency for the core decision); a gtest
  drives "simulate crash → recovery file present → decision picks recover" with
  a fake clock. The timer is a thin widget-side wrapper.

## 4. esmini round-trip CI gate (permanent from M3a)

M3a owns the roadmap's
[simulator round-trip gate](../../roadmap/roadmap.md#cross-cutting-quality-gates):
every golden scene's exported `.xodr` must **load headless in esmini without
errors**.

- **Job:** a new CI job `esmini-roundtrip` that, for each golden `.xodr`
  (GS-1 first), runs esmini headless (`--headless`, minimal `.xosc` wrapper or
  esmini's odr-viewer load-check) and fails on any parse/load error.
- **License/distribution check (mandatory, risk 5):** verify esmini's license
  and binary-distribution method at implementation time against the
  [dependency policy](../../standards/dependencies.md). esmini is **not linked**
  — it is an external smoke tool fetched as a pinned release binary in CI (like a
  test fixture), not a build dependency, so its license gates *usage in CI*, not
  the shipped product. If the binary distribution is not policy-clean, fall back
  to esmini's OpenDRIVE reader invoked as a subprocess from a pinned source
  build; record the decision in the PR.
- **Scope:** load-only smoke (geometry + network parse). CARLA ingestion stays a
  manual release-checklist item (not CI-feasible yet), per the seed.
- The job is permanent: every future golden scene is added to it.

## 5. Minimal user guide (`docs/user-guide/`)

Ships with v0.4.0, kept separate from contributor docs.

- **Structure:** `docs/user-guide/index.md` + one page per tool
  (create-road, edit-nodes, lane-profile, elevation, junction, objects-signals,
  library-panel, save-export), each with a short task description and a
  screenshot from the current build.
- **Scope:** tool-by-tool "how to author GS-1"-level guidance, not a reference
  manual. Screenshots are generated assets (recorded in `ASSETS_LICENSES.md` as
  original works, like the M2 icons).
- **Link:** referenced from `README.md` and the v0.4.0 release notes; the docs
  link-check CI job must pass over it.

## 6. Test plan

- `LibraryListModel` + any new item model: `QAbstractItemModelTester` gtest in
  the same commit.
- Placement commands: headless apply→revert byte-identical `write_xodr`
  round-trip (the M2 invariant) for every new command.
- Drag-to-place: simulated drop at a known point resolves to the expected
  snapped `s`/`t` and creates one command (undo removes exactly it).
- Autosave/recovery: fake-clock gtest for the recover-vs-clean decision; no
  overwrite of the user file.
- esmini job: GS-1 export loads clean; a deliberately-broken `.xodr` fixture
  fails the job (guards the gate itself).
- User guide: docs link-check green; every referenced screenshot exists and has
  a license row.
</content>
