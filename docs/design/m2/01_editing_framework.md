# M2 editing framework — commands, tools, snapping, incremental re-mesh

Design constraints (non-negotiable, from CLAUDE.md and the M2 prompt):

- Every mutation is undoable. Kernel mutations go through `Document` methods that
  create commands; **widgets never mutate the network directly**.
- Commands store kernel-level deltas (IDs + serialized state), never widget
  state — undo survives panel changes and selection changes.
- `core/` stays UI-free: the command layer lives in the kernel (namespace
  `roadmaker::edit`), Qt sees only a thin bridge.
- All new public kernel API carries `RM_API` and returns `rm::Expected`; no
  exceptions across the kernel boundary.

## 1. Layering: one stack, kernel commands

```
widgets ──▶ Document (Qt) ──▶ QUndoStack (the ONLY stack in the editor)
                                  │ push(EditorCommand)
                                  ▼
                    EditorCommand : QUndoCommand   (editor/src/document/)
                        owns std::unique_ptr<roadmaker::edit::Command>
                        redo() = cmd->apply(network)   undo() = cmd->revert(network)
                                  │
                                  ▼
                    roadmaker::edit::Command       (core/, UI-free)
```

**Decision — the editor does NOT get a second, kernel-side stack.** The kernel
provides `Command` objects (the deltas); Qt's `QUndoStack` (already owned by
`Document` and wired to Edit→Undo/Redo since M1.5) is the single stack. A
kernel-side `EditStack` (simple vector + cursor over the same `Command` type) is
provided **for Python and headless use only**, so `import roadmaker` gets
undo/redo parity without Qt. Rationale: two live stacks for one document is a
divergence hazard (depth limits, macro grouping, clear-on-load would need to be
mirrored); QUndoStack already handles action enable/disable, macros, and
compression hooks.

### 1.1 Kernel command API (exact signatures)

```cpp
// core/include/roadmaker/edit/command.hpp
namespace roadmaker::edit {

struct DirtySet {
  std::vector<RoadId>     roads;      // need re-mesh
  std::vector<JunctionId> junctions;  // need floor/surface regeneration
  bool topology = false;              // roads/junctions added or removed
};

class RM_API Command {
public:
  virtual ~Command() = default;
  virtual Expected<void> apply(RoadNetwork& network) = 0;
  virtual Expected<void> revert(RoadNetwork& network) = 0;
  virtual std::string_view name() const = 0;   // for undo menu text
  virtual DirtySet dirty() const = 0;          // valid after apply/revert
};

}  // namespace roadmaker::edit
```

Contract: `apply` then `revert` restores the network to a state whose
`write_xodr()` output is **byte-identical** to the pre-apply output (the writer
is deterministic — this is the round-trip test oracle). `apply` after a failed
apply is undefined; a failed `apply` must leave the network unchanged (validate
first, mutate after — commands do all fallible work up front).

Concrete commands are created by factory functions, one per edit operation
(§2.3). They capture *values*, never pointers (arena pointers are invalidated by
any mutation) and never widget state.

### 1.2 Headless stack (Python parity)

```cpp
// core/include/roadmaker/edit/edit_stack.hpp
class RM_API EditStack {
public:
  Expected<void> push(RoadNetwork&, std::unique_ptr<Command>);  // applies, records
  Expected<void> undo(RoadNetwork&);
  Expected<void> redo(RoadNetwork&);
  bool can_undo() const;  bool can_redo() const;
  void clear();
  void set_depth_limit(std::size_t);   // drops oldest; default 256
};
```

Bound in `python/src/bindings.cpp` with an `edit_network.py` example (same PR as
the kernel command layer, per the bindings-parity rule).

### 1.3 Qt bridge

```cpp
// editor/src/document/editor_command.hpp
class EditorCommand final : public QUndoCommand {
public:
  EditorCommand(Document& doc, std::unique_ptr<roadmaker::edit::Command> cmd,
                bool already_applied);
  void redo() override;   // skips the first call when already_applied (preview commit)
  void undo() override;
};
```

`redo()`/`undo()` call the kernel command, then `Document` re-meshes the dirty
set and emits `mesh_changed()` (and `topology_changed()` when `dirty().topology`
— new signal, drives SceneTreeModel reset). `Document::push_command()` is the
only entry point; it is also where a failed apply is surfaced (status bar +
Diagnostics panel) instead of being pushed.

## 2. Transactional mutation strategy

### 2.1 Decision: per-command value snapshots (not network-wide COW)

Each command captures by-value copies of exactly the domain objects it touches
(`Road`, `LaneSection`, `Lane`, `Junction` are plain value types — cheap at this
scale). `revert` writes the copies back **in place** (same arena slots).
Rejected alternatives:

- *Whole-network snapshot per command*: O(network) memory per edit; kills the
  depth-256 stack on large files.
- *Copy-on-write network*: invasive to the arena design and to every accessor;
  value snapshots achieve the same rollback with local changes only.

### 2.2 Arena additions (restore-in-place)

Undoing an erase must resurrect an object **with its original ID** (other
objects hold that ID). Undoing a create must free the slot without invalidating
unrelated IDs. The arena gains two `RM_API` methods:

```cpp
// Arena<T, IdT>
Expected<IdT> restore(IdT id, T value);  // re-occupies id.index, requires the slot
                                         // free and gen == id.gen (no gen bump)
Expected<void> erase_exact(IdT id);      // erase that does NOT bump generation,
                                         // paired only with restore() by commands
```

Invariant: `restore`/`erase_exact` are for the command layer only (documented;
normal `erase` still bumps generations). Round-trip tests assert that IDs held
by other objects (links, junction connections) remain valid across undo/redo.

### 2.3 Edit operations (kernel factories, all `RM_API`, all return `Expected`)

```cpp
namespace roadmaker::edit {
// geometry (re-fit clothoid through authoring waypoints, §2.5)
std::unique_ptr<Command> move_waypoint(const RoadNetwork&, RoadId, std::size_t index, Waypoint to);
std::unique_ptr<Command> insert_waypoint(const RoadNetwork&, RoadId, std::size_t index, Waypoint at);
std::unique_ptr<Command> delete_waypoint(const RoadNetwork&, RoadId, std::size_t index);
// topology
std::unique_ptr<Command> create_road(std::vector<Waypoint>, LaneProfile, std::string name);
std::unique_ptr<Command> split_road(const RoadNetwork&, RoadId, double s);
std::unique_ptr<Command> delete_road(const RoadNetwork&, RoadId);          // detaches junction links, restores all on undo
std::unique_ptr<Command> create_junction(const RoadNetwork&, std::span<const RoadEnd>);  // 02 §7 / 03 §2
std::unique_ptr<Command> delete_junction(const RoadNetwork&, JunctionId);
// lanes
std::unique_ptr<Command> add_lane(const RoadNetwork&, LaneSectionId, int side /*+1 left, -1 right*/, LaneType);
std::unique_ptr<Command> remove_lane(const RoadNetwork&, LaneId);
std::unique_ptr<Command> set_lane_type(const RoadNetwork&, LaneId, LaneType);
std::unique_ptr<Command> set_lane_width(const RoadNetwork&, LaneId, double width_m);   // constant width in M2
std::unique_ptr<Command> set_road_mark(const RoadNetwork&, LaneId, RoadMark);
// profiles
std::unique_ptr<Command> set_node_elevation(const RoadNetwork&, RoadId, std::size_t waypoint_index, double z);
// document
std::unique_ptr<Command> rename_road(const RoadNetwork&, RoadId, std::string name);
}
```

`RoadEnd` is `{RoadId road; ContactPoint contact;}` (new small struct in
`road/road.hpp`). Factories take `const RoadNetwork&` to capture snapshots and
validate up front; `apply` re-validates cheaply (generational IDs catch stale
targets and return `ErrorCode::InvalidArgument`).

### 2.4 Junction dependency tracking

Editing a road that is an incoming road of a junction must regenerate that
junction (connecting-road re-fit + surface). The kernel gains a reverse-lookup
query `junctions_touching(RoadNetwork&, RoadId) → small_vector<JunctionId>`
(linear scan in M2 — junction counts are small; spatial index is the M2
performance step's problem if it ever shows up). Command factories call it to
fill `DirtySet::junctions`; regeneration itself is a deterministic function of
the incoming roads (03 §5), so undo does not need to snapshot generated meshes,
only the junction's topology record.

### 2.5 Authoring waypoints as Road metadata

`Road` gains `std::optional<std::vector<Waypoint>> authoring_waypoints`. Set by
the authoring API and edit commands; **persisted** in `.xodr` via a
`<userData code="rm:waypoints">` extension element (spec-sanctioned
`<userData>`, see OpenDRIVE 1.9.0 §7 Additional data) so an edit session
round-trips through save/load. For roads loaded without it (foreign files),
Edit Nodes derives waypoints lazily from geometry-record endpoints (each record
start + final endpoint) — the first node edit re-fits the whole reference line
as a clothoid path through those points and records the result (this is a
geometry-altering operation within `tol::kRoundTripPosition` only for pure
line/arc/spiral chains; the Properties panel shows a one-time notice for
paramPoly3 roads). The writer must keep emitting foreign geometry untouched for
roads that were never node-edited.

## 3. Drag interactions: preview session + one command

**Decision: no `mergeWith`, no `setObsolete`.** During a drag the tool runs a
*preview session*; exactly one command is pushed, on release:

```cpp
// Document (editor)
using PreviewFactory = std::function<std::unique_ptr<edit::Command>(const RoadNetwork&)>;
Expected<void> begin_preview(std::unique_ptr<edit::Command>); // applies, remeshes dirty
Expected<void> update_preview(const PreviewFactory&); // reverts current, builds vs base, applies
void commit_preview();  // pushes EditorCommand(already_applied=true) onto QUndoStack
void cancel_preview();  // reverts, discards (Esc)
```

`update_preview` takes a factory rather than a ready command because command
factories snapshot at creation time (§2.3): a command created while the
previous preview frame was still applied would capture that frame — not the
base state — as its "before" values, and undo after commit would restore
mid-drag geometry. Document reverts the current command first and invokes
the factory against the restored base-state network.

Rationale: `mergeWith` compresses *pushed* commands, so every drag frame would
enter the stack and signal listeners before merging — wasted work and
subtle-bug surface (macro boundaries, depth-limit interplay). The preview
session keeps the stack clean by construction, gives live re-mesh through the
same dirty-set path, and makes Esc-cancel trivial. `EditorCommand`'s
`already_applied` flag skips the first `redo()` QUndoStack fires on push.
Invariant tests: a preview session that is cancelled leaves `write_xodr()`
byte-identical; a commit leaves exactly one stack entry.

## 4. Tool state machine

```
ViewportWidget (Qt events, GL)          ToolManager (QObject, editor/src/tools/)
  mouse/key events ──▶ ToolEvent ──▶ active Tool ──▶ Document::push_command / preview
  draws Tool::preview() overlays ◀── preview_changed()
```

```cpp
// editor/src/tools/tool.hpp  (QtCore only — headless-testable)
struct ToolEvent {
  Vec2d world;                       // cursor ray ∩ z=0 plane (kernel frame)
  std::optional<PickHit> pick;       // lane-patch hit if any
  std::optional<SnapResult> snap;    // §6, computed by ToolManager
  Qt::MouseButtons buttons;
  Qt::KeyboardModifiers modifiers;
};

class Tool : public QObject {
public:
  virtual void activate();  virtual void deactivate();   // reset state machine
  virtual bool mouse_press(const ToolEvent&);   // true = consumed
  virtual bool mouse_move(const ToolEvent&);
  virtual bool mouse_release(const ToolEvent&);
  virtual bool key_press(int key, Qt::KeyboardModifiers);
  virtual PreviewGeometry preview() const;      // line/point sets, kernel frame
signals:
  void preview_changed();
  void status_message(const QString&);
};

class ToolManager : public QObject {
public:
  ToolManager(Document&, SelectionModel&);
  void set_active(ToolId);            // deactivates old, activates new; Esc→Select
  Tool& active();
signals:
  void active_changed(ToolId);
};
```

- Tools receive **abstract events** (world positions, picks, snaps, modifiers) —
  never `QMouseEvent`, never GL. Gtest drives tools headless by feeding
  `ToolEvent` sequences and asserting on pushed commands + preview geometry
  (this is the test seam the M2 prompt requires).
- `ViewportWidget` keeps camera navigation and event translation only. Button
  map changes in M2: **LMB = active tool, RMB-drag = orbit, MMB-drag = pan,
  wheel = zoom** (LMB-orbit moves to RMB; Select tool preserves click-select and
  adds rubber band). `PreviewGeometry` is uploaded via the existing `Renderer`
  `PrimitiveKind::Lines` path — no renderer interface change.
- Tool set: `Select` (default), `CreateRoad`, `EditNodes`, `LaneProfile`
  (panel-driven, tool sets viewport hint mode only), `Elevation`,
  `CreateJunction`, `Delete`. Toolbar `QAction`s in an exclusive
  `QActionGroup`; icons per `05_assets.md`.

## 5. Incremental re-mesh

New kernel entry point (meshing stays a pure function of the network):

```cpp
// mesh/mesh_builder.hpp
RM_API void remesh_roads(const RoadNetwork&, NetworkMesh& mesh,
                         std::span<const RoadId> roads, const MeshOptions& = {});
RM_API void remesh_junctions(const RoadNetwork&, NetworkMesh& mesh,
                             std::span<const JunctionId> junctions, const MeshOptions& = {});
```

`remesh_roads` replaces the matching `RoadMesh` entries in place (or
appends/removes on topology change); untouched roads keep their vertex buffers.
`Document` maps the dirty set to these calls, then tells the viewport which
roads changed via `mesh_changed(std::vector<RoadId>)` (signal gains a payload;
empty = everything, preserving the load path). The viewport re-uploads only
those roads' `UploadedItem`s and updates their cached AABBs (also fixing the
M1 `frame_selection` rebuild seam).

**Performance budget:** node drag on a 100-road network re-meshes the affected
roads in **< 16 ms** per preview update on the dev machine (Apple Silicon,
Release). Measurement plan: informational gtest benchmark
(`test_remesh_budget.cpp`) builds a generated 100-road grid network, times
`remesh_roads` for 1 road (median of 100 runs via `std::chrono::steady_clock`),
prints the number, and asserts only a generous ceiling (160 ms) so CI noise
never gates — the real budget is tracked by eye until M2's performance step.
Supporting change: per-record clothoid evaluator cache (the rev-2 plan's
`eval_spiral` seam) lands with this work since dragging makes evaluation hot.

## 6. Snapping subsystem (kernel-side, pure)

```cpp
// core/include/roadmaker/edit/snap.hpp
namespace roadmaker::edit {
enum class SnapKind { Grid, RoadEndpoint, TangentContinuation };
struct SnapOptions {
  double radius = 2.0;              // world meters, screen-scaled by caller
  std::optional<double> grid;       // grid spacing, nullopt = off
  bool endpoints = true;
  bool tangent = true;
};
struct SnapResult {
  Vec2d position;
  std::optional<double> heading;    // set for TangentContinuation
  SnapKind kind;
  std::optional<RoadId> road;       // source road for endpoint/tangent
};
RM_API std::optional<SnapResult> snap_point(const RoadNetwork&, Vec2d cursor,
                                            const SnapOptions&);
}
```

Priority on conflict: endpoint > tangent-continuation > grid (closest wins
within a kind). Tangent-continuation returns the position **and heading** of a
road end so Create Road can chain G1-continuously. Pure function → direct
gtests with hand-built networks; the viewport only renders the returned hint
(marker + ghost tangent line). Grid snapping is included here (not in the
editor) so headless tool tests cover the full interaction.

## 7. Selection and editable Properties

- **SelectionModel goes multi-select.** State becomes an ordered
  `std::vector<SelectionEntry>` where
  `SelectionEntry{RoadId road; LaneId lane /*invalid = road-level*/}`, plus a
  *primary* entry (the last-selected, drives the Properties panel). API:
  `select(entry, SelectMode {Replace, Toggle, Add})`, `select_many(...)` (rubber
  band), `clear()`, `entries()`, `primary()`, `contains(entry)`. Single signal
  `selection_changed()` (listeners query state; the old two-arg signal is
  removed — all call sites updated in the same commit). Existing behavior
  preserved: hard-clear on `Document::loaded()`; QAbstractItemModelTester-style
  invariants get a dedicated gtest (no duplicate entries, primary ∈ entries).
- **Properties panel becomes editable via manual binding** (decision:
  no `QDataWidgetMapper` — it targets `QAbstractItemModel`-backed rows; our
  source is kernel structs reached through IDs, and mapping would force a
  throwaway model). Pattern: each editable row is a small widget
  (`QLineEdit`/`QDoubleSpinBox`/`QComboBox`) whose `editingFinished`/`activated`
  handler builds the corresponding kernel command and calls
  `Document::push_command()`; the panel refreshes from `mesh_changed`/undo via
  the same `refresh()` it has today. A re-entrancy guard (`updating_` flag)
  prevents echo loops. Editable in M2: road name; lane type, constant width,
  road-mark type/width. Everything else stays read-only labels.
- **SceneTreeModel:** reset-on-command-boundary in M2 (listen to
  `topology_changed()`; plain `mesh_changed` doesn't touch the tree). Full
  incremental `rowsInserted/rowsRemoved` is deferred — command granularity makes
  resets rare (topology edits only), and the model tester keeps passing.

## 8. Test plan (framework level)

All GoogleTest; editor parts offscreen. With the code, not after.

| Area | Tests |
|---|---|
| Command round-trip | For EVERY factory in §2.3: apply→revert ⇒ `write_xodr` byte-equal; apply→revert→apply ⇒ byte-equal to single apply (idempotence) |
| Restore-in-place | delete_road undo keeps junction connection IDs valid; create undo frees slot; gen not bumped through command path |
| EditStack | depth limit drops oldest; redo cleared on push; clear() |
| EditorCommand bridge | QUndoStack push applies once (already_applied honored); undo/redo drive kernel; failed apply not pushed; stack cleared on load |
| Preview session | cancel ⇒ byte-equal; commit ⇒ exactly 1 stack entry; update N times ⇒ no leak into stack |
| Dirty propagation | editing incoming road marks its junction dirty; remesh_roads touches only listed roads (pointer identity of untouched buffers) |
| Snapping | endpoint/tangent/grid priorities, radius edge, heading correctness vs `ReferenceLine::evaluate` |
| Tools | per-tool headless event-sequence tests (02 spec sheets) |
| SelectionModel | multi-select invariants, rubber-band add, clear-on-load |
| Benchmark | remesh budget (informational, generous ceiling) |

A shared test util (`core/tests/support/network_compare.hpp`, promoted from
`test_round_trip.cpp`) provides `expect_networks_equal(a, b)` (writer
byte-compare) and `expect_same_geometry(road, road)` (tolerance compare).
