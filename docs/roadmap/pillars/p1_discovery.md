# P1 discovery — Interaction & Navigation

*What the code actually looks like today, per P1 scope item, and what that
means for the sprint cut. Written 2026-07-15, before p1-s1 landed. Roadmap:
[Road to Parity](../README.md) · Acceptance:
[GW-1](../golden_workflows/gw1_camera.md).*

> **Status (2026-07-23):** the pillar was sprint-complete; it is **reopened**
> by the [2026-07 field triage, batch 2](../updates/2026-07-field-triage-2.md)
> (#397) for three release-blocking items: bug
> [#401](https://github.com/Robomous/RoadMaker/issues/401) (gizmo
> junction-arm refusal is silent),
> [#404](https://github.com/Robomous/RoadMaker/issues/404) (`p1-f7`:
> below-ground navigation), and
> [#405](https://github.com/Robomous/RoadMaker/issues/405) (`p1-f8`:
> viewport axis-navigation widget). GW-1 hand-run status is unaffected until
> the items land (#404/#405 amend GW-1 with their PRs).

## Why this document exists

The P1 sprint bodies were written from the roadmap, not from the code. Three
of them described work that is already done, already different, or riskier
than the wording suggested. This records the ground truth each sprint builds
on, so the sprint scopes can be read against reality.

## 1. Camera — already an orbit-pivot camera

`editor/src/viewport/camera.{hpp,cpp}` is **already a pure headless
orbit-pivot camera**: `OrbitCamera` holds `target_` / `yaw_` / `pitch_` /
`distance_`, orbits with a pitch clamp, and is unit-tested headlessly in
`editor/tests/test_camera.cpp`. It has no Qt dependency.

**#210's "replace the free camera" wording is wrong** — there is no free
camera to replace. p1-s1 is pivot semantics plus input chords, not a
rewrite. The camera keeps its shape; what it gains is a 1.5 m default pivot,
a vertical pivot move, and a hoisted FOV constant.

Missing against P1 scope:

| Scope item | State today |
|---|---|
| 1.5 m default pivot | `target_` defaults to `{0,0,0}` |
| Alt chords | no modifier chords at all (see §4) |
| Vertical pivot move | no API — `move_target` is x/y only |
| Push-past-pivot zoom | `zoom()` dead-stops at `kMinDistance` |
| Orthographic | projection hardcoded 50° perspective in `matrices()`; the FOV literal is **duplicated** in `pan_pixels()` |
| Cardinal views | only a scripting-only `set_camera_preset("top"/"orbit"/"gs1")` |

## 2. Framing — exists, coarse, and buggy

`F` = frame-selection exists (`viewport_widget.cpp:1352+`), but it copies
whole road/object meshes into a temporary `NetworkMesh` and frames that. The
consequences are visible bugs, not just imprecision:

- selecting a **signal** frames its entire road;
- **junction** selections are ignored outright;
- selecting a **lane** frames the whole road.

There is no frame-on-cursor. p1-s2 replaces the temp-mesh copy with per-kind
bounds, which fixes all three as a side effect.

## 3. Projections / cardinal views — greenfield

Nothing exists. The good news is that the two things most likely to break
under a projection change do not: **picking**
(`viewport/picking.{hpp,cpp}`, which unprojects via `inverse(proj*view)`)
and the gizmo screen math are both already projection-agnostic — verified by
reading, and guarded by `test_picking` / `test_projection` once p1-s2 adds
ortho cases.

## 4. Input — chords are free

`ViewportWidget` overrides Qt events directly and hardcodes precedence
inline: gizmo→tool for LMB; RMB = orbit/context, MMB = pan, wheel = zoom.
Tools only ever receive **LMB + key_press** — **no tool uses Alt, MMB, or
the wheel**. The Alt chord space is therefore unoccupied, which makes p1-s1
a low-regression change: the plain-LMB gizmo→tool path can stay
byte-identical while chords are intercepted ahead of it.

## 5. Attributes pane — scrub and slots are greenfield

`panels/properties_panel.{hpp,cpp}` is hand-coded per-kind sections. Edits
already push an `edit::Command` through `Document::push_command`, and the
preview-session pattern (`begin_preview` / `update_preview` /
`commit_preview` / `cancel_preview`) already delivers one-undo-entry
gestures — so scrub-editing has the transaction model it needs and only
needs a widget.

Library drag MIME is `"application/x-roadmaker-library-item"`, payload =
item key as UTF-8 (`document/library_list_model.hpp`) — that is the contract
the p1-s3 slot widget accepts.

## 6. 2D Editor host — greenfield, clean migration

`panels/profile_panel.{hpp,cpp}` is a plain `QWidget` that MainWindow wraps
in `profile_dock_` (objectName `dock.profile`, bottom, hidden by default).
It subscribes to selection itself in its constructor, so hosting it behind a
tabbed interface does not touch its logic and
`editor/tests/test_profile_panel.cpp` should pass unmodified.

## 7. Status bar — transient only

Per-tool `status_message` exists but is transient (5 s `showMessage`),
alongside a viewport hint and a tool-options tooltip echo. P1 needs a
**persistent** instruction line; the transient channel stays for
state-dependent guidance.

## 8. Shortcuts — central registry, decentralized truth

`app/actions.{hpp,cpp}` is already a central `Actions` registry, but every
shortcut is hardcoded inline (`tool_select->setShortcut(Qt::Key_V)`), key
letters are duplicated by hand into tooltips, no shortcut page exists in the
user guide, and CI has no doc-consistency gate (only a lychee link check).

## Decisions this discovery forced

**Key conflict: `V` is taken.** GW-1 step 10 wants `V` = frame-on-cursor;
`V` is the Select tool today. **Decision (Armando, 2026-07-15): Select
rebinds to `Q`**, `V` becomes frame-on-cursor. Lands in p1-s2, documented in
the p1-s4 registry and shortcut page.

**Bounds: no `core/` changes.** There is no `Aabb` anywhere in `core/`; the
editor has `SceneBounds` (`render/scene_builder.hpp`) built from meshes.
**Decision: per-kind bounds are computed editor-side from `NetworkMesh`** —
the meshes already carry everything framing needs, so the kernel stays out
of a viewport concern.

**Slot consumer uses a kernel factory.** The p1-s3 "Model" slot commits
through a new `edit::set_object_model` (~40 lines, value-snapshot pattern)
rather than an editor-side undo macro. Per repo policy that pulls
`python/src/bindings.cpp` and one `python/examples/` update into the same
PR.

## GW-1 amendments these findings propose

GW-1 is marked draft ("steps are refined as the P1 sprints land"). The
proposed amendments, landing with the sprints that implement them:

1. **Step 13**: top-down is near-vertical (pitch = π/2 − 0.01 ≈ 89.4°) to
   keep the look-at basis non-degenerate. Indistinguishable on screen.
2. **Step 10**: `V` preserves the current zoom distance — the pivot moves,
   no dolly.
3. **Step 12**: cardinals snap **yaw only**, preserving pitch, distance and
   pivot.
4. **Step 3**: Alt+RMB drag-up = zoom in (direction convention).
5. **Bindings preamble**: record the Select→`Q` rebind, and top-row digits
   8/2/4/6/5 as the numpad-less alternates.

## Sprint cut this implies

| Sprint | Reality-adjusted scope |
|---|---|
| [#210](https://github.com/Robomous/RoadMaker/issues/210) p1-s1 | Pivot semantics + Alt chord state machine (`nav_controller`) — **not** a camera rewrite |
| [#211](https://github.com/Robomous/RoadMaker/issues/211) p1-s2 | Projection mode, push-past, per-kind framing (fixes §2 bugs), cardinals, Select→Q |
| [#212](https://github.com/Robomous/RoadMaker/issues/212) p1-s3 | Scrub widget + slot widget on the existing preview-session model; kernel `set_object_model` |
| [#213](https://github.com/Robomous/RoadMaker/issues/213) p1-s4 | 2D Editor host around the existing ProfilePanel, persistent status line, shortcut registry + generated doc + CI gate |
