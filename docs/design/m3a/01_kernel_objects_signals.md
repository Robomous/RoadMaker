# M3a kernel — OpenDRIVE `<objects>` and `<signals>`

Design constraints (non-negotiable, from CLAUDE.md and the M3a seed):

- `core/` stays UI/GL/Qt/Python-free. New types live in
  `core/include/roadmaker/road/`.
- Arena storage + generational strong IDs; domain objects reference each other
  by ID, never by pointer.
- The parser **never silently drops input**: every `<object>`/`<signal>` that
  parses becomes a domain object; anything M3a doesn't model semantically is
  preserved verbatim for round-trip (§5) and, where a normative rule exists, a
  `Diagnostic` with the rule ID is emitted — not a drop.
- Every standards claim below cites the local ASAM text; §7 summarizes the
  mandated read of §13/§14.
- Kernel frame: right-handed, Z-up, meters, radians. Object/signal placement is
  in road `s`/`t`/`zOffset`, resolved to world through the existing reference
  line + elevation evaluation.

This doc covers the data model, parse, write, validation, and mesh anchoring for
objects and signals. Road-mark completions (which the spec models partly as
object markings) are in [`02`](02_road_marks.md).

## 1. Scope — what M3a *authors* vs. what it *round-trips*

OpenDRIVE §13/§14 are large. M3a splits every element into three tiers:

| Tier | Meaning | M3a coverage |
|---|---|---|
| **Authored** | Editor can create/edit; validated; meshed | GS-1 set only (below) |
| **Modeled** | Parsed into typed fields, written back, validated | The common `<object>`/`<signal>`/`<repeat>`/`<outline>` attributes |
| **Preserved** | Not interpreted; kept verbatim so round-trip loses nothing | Everything else (unknown attributes, `<skeleton>`, `<curveLocal>`, `<userData>`, signal `<dependency>`, `<controller>` links) |

The **authored** (GS-1) set, per
[the golden scene](../../roadmap/golden_scenes/gs1_urban_intersection.md):

- **Objects:** `crosswalk` (outline object, one per arm), point props — `tree`
  / `vegetation` (point object + `<repeat>` for tree lines), `pole`
  (signal/sign carrier).
- **Signals:** one **dynamic** traffic light per approach; **static**
  speed-limit and pedestrian-crossing-warning signs on ≥2 arms.

Everything outside this set is *modeled* or *preserved*, never dropped. This
holds risk 1 (`00` risk register) at bay: the data model is exactly wide enough
for GS-1, and foreign files survive unchanged.

## 2. Data model

New headers under `core/include/roadmaker/road/`. IDs extend the existing
generational-ID scheme in `road/id.hpp`.

### 2.1 IDs and ownership

```cpp
// road/id.hpp — new strong ids (same generational template as RoadId)
using ObjectId = GenerationalId<struct ObjectTag>;
using SignalId = GenerationalId<struct SignalTag>;
```

OpenDRIVE defines objects and signals **per `<road>`** (§13.1, §14.1). To keep
the arena-owns-everything invariant (M2), the arenas live on `RoadNetwork`
alongside roads/lanes/junctions, and each `Object`/`Signal` carries a
back-reference `RoadId` (mirroring `Lane::section`). Iteration for a road is a
filtered scan; at GS-1 scale (tens of objects) this is fine, and it keeps
`Road` a value type that copies cheaply for the command layer.

```cpp
class RoadNetwork {
  // ... existing arenas ...
  Arena<Object, ObjectId> objects_;
  Arena<Signal, SignalId> signals_;
public:
  RM_API ObjectId add_object(RoadId road, Object value);
  RM_API SignalId add_signal(RoadId road, Signal value);
  RM_API bool erase_object(ObjectId id);   // cascades nothing (leaf)
  RM_API bool erase_signal(SignalId id);
  // restore/erase_exact pairs for the command layer, per the M2 pattern
  RM_API Expected<ObjectId> restore_object(ObjectId id, Object value);
  RM_API Expected<void> erase_object_exact(ObjectId id);
  // ... signal equivalents ...
  [[nodiscard]] Object* object(ObjectId id);
  [[nodiscard]] Signal* signal(SignalId id);
  template <class Fn> void for_each_object(Fn fn) const;  // (RoadId filter helper)
  template <class Fn> void for_each_signal(Fn fn) const;
};

// objects/signals a road owns — leaf cleanup on erase_road (see §2.4)
[[nodiscard]] RM_API std::vector<ObjectId> objects_of(const RoadNetwork&, RoadId);
[[nodiscard]] RM_API std::vector<SignalId> signals_of(const RoadNetwork&, RoadId);
```

### 2.2 `Object`

```cpp
// road/object.hpp
enum class ObjectType {                 // e_objectType subset RoadMaker authors
  None, Crosswalk, Tree, Vegetation, Pole, Barrier, Building, Obstacle, Other,
};

enum class ObjectOrientation { Plus, Minus, None };  // e_orientation, §13.1

/// One <cornerRoad> or <cornerLocal> outline vertex. `road_coords` selects
/// which pair is authoritative (mutually exclusive per §13.2.1/§13.2.2).
struct OutlineCorner {
  double a = 0.0;      // s (road) or u (local)
  double b = 0.0;      // t (road) or v (local)
  double height = 0.0; // object height at this corner [m]
  double dz_or_z = 0.0;// dz (cornerRoad) or z (cornerLocal)
  std::optional<int> id;
};

struct ObjectOutline {
  bool road_coords = true;   // true=<cornerRoad>, false=<cornerLocal>
  bool closed = false;       // §13.2 — area vs. line feature
  bool outer = true;         // exactly one outer per object (rule cited §4)
  std::optional<int> id;
  std::optional<std::string> fill_type;   // e_outlineFillType, e.g. "paint"
  std::optional<LaneType> lane_type;
  std::vector<OutlineCorner> corners;
};

/// <repeat> — object series (tree lines, railings), §13.4.
struct ObjectRepeat {
  double s = 0.0, length = 0.0, distance = 0.0;
  double t_start = 0.0, t_end = 0.0;
  double z_offset_start = 0.0, z_offset_end = 0.0;
  std::optional<double> width_start, width_end, height_start, height_end,
                        length_start, length_end, radius_start, radius_end;
};

struct Object {
  RoadId road;                 // owning road (back-reference)
  std::string odr_id;          // <object @id> — unique in file (string)
  std::string name;            // optional
  ObjectType type = ObjectType::None;
  std::string subtype;         // free string variant, §13.1
  double s = 0.0, t = 0.0;     // origin in road coords (required)
  double z_offset = 0.0;       // required, §13.1
  double hdg = 0.0, pitch = 0.0, roll = 0.0;
  ObjectOrientation orientation = ObjectOrientation::None;
  bool perp_to_road = false;   // 1.7.0; overrides pitch/roll if true
  // bounding volume — angular (length/width) XOR circular (radius); §13.1
  std::optional<double> length, width, radius;
  double height = 0.0;
  double valid_length = 0.0;   // 0 for point object
  std::optional<bool> temporary, invalidated;   // 1.9.0
  std::vector<ObjectOutline> outlines;          // crosswalks / painted markings
  std::optional<ObjectRepeat> repeat;           // tree lines
  RawXml preserved;            // §5 — verbatim <skeleton>, unknown attrs/children
};
```

### 2.3 `Signal`

```cpp
// road/signal.hpp
struct Signal {
  RoadId road;
  std::string odr_id;          // <signal @id> — required, unique in file
  std::string name;            // optional
  double s = 0.0, t = 0.0;     // required
  double z_offset = 0.0;       // required, §14.1
  bool dynamic = false;        // required (@dynamic yes/no) — light vs. sign
  ObjectOrientation orientation = ObjectOrientation::None;  // required
  double h_offset = 0.0, pitch = 0.0, roll = 0.0;
  std::string type, subtype;   // required; country-coded. GS-1 set in §3
  std::string country;         // e_countryCode; "OpenDRIVE" for catalog signals
  std::string country_revision;
  std::optional<double> value; // e.g. 50 for a speed limit; unit then required
  std::string unit;            // e_unit; mandatory iff value present (§4)
  std::optional<double> height, width, length;   // length is 1.8.0
  std::string text;
  std::optional<bool> temporary, invalidated;    // 1.9.0
  // validity / references preserved verbatim in M3a (not authored):
  RawXml preserved;            // <validity>, <dependency>, <reference>, <userData>
};
```

`RawXml` is a small opaque holder (an owned XML fragment string or a light DOM
copy) already needed for foreign passthrough; it is defined once in
`xodr/raw_xml.hpp` and reused by both structs (§5).

### 2.4 Lifecycle & command layer

- Objects and signals are **leaves**: nothing references them, so `erase_object`
  / `erase_signal` bump generations and cascade nothing. `erase_road` must also
  erase the road's owned objects/signals (extend the existing cascade in
  `RoadNetwork::erase_road`), and the Delete command captures their values so
  undo restores them (`restore_object`/`restore_signal`, no generation bump —
  the M2 restore-in-place contract).
- New edit commands (`edit/operations.hpp`): `AddObject`, `RemoveObject`,
  `MoveObject` (s/t/zOffset/hdg), `AddSignal`, `RemoveSignal`, `MoveSignal`,
  `SetSignalValue`. Each captures values, never arena pointers, and satisfies
  the M2 invariant: apply→revert leaves `write_xodr()` byte-identical.
- Dirty propagation: an object/signal edit marks only its owning road's
  **object layer** dirty (a new `DirtySet::objects` set keyed by `RoadId`), so
  re-mesh re-anchors props without re-tessellating road surfaces (`04` §4).

**Phase ownership (clarified 2026-07-11, after Phase 0 shipped without them):**
nothing in this subsection belongs to phases 0/1. The `edit::Command` wrappers
(`AddObject` … `SetSignalValue`) land in **phase 5**
([#72](https://github.com/Robomous/RoadMaker/issues/72)) together with their
Python bindings and headless tool tests; `DirtySet::objects` lands in
**phase 2** ([#69](https://github.com/Robomous/RoadMaker/issues/69)) with its
first mesh consumer. Phases 0/1 ship arena + parse/write/validate only — a
phase-1 executor must **not** add signal commands "for parity".

## 3. GS-1 signal & object catalog (hard-coded, decision 1)

M3a ships **no signal-catalog data file**; it hard-codes exactly the GS-1 set as
`type`/`subtype`/`country` constants in `core/src/xodr/signal_catalog.hpp`
(kernel-internal; the editor's placement UI reads the same table). Country
catalogs are backlog. GS-1 uses German (`DE`) codes, the OpenDRIVE reference
default for examples:

| GS-1 element | dynamic | type | subtype | value/unit | Source |
|---|---|---|---|---|---|
| Traffic light | yes | 1000001 | -1 | — | OpenDRIVE Signal reference (`country="OpenDRIVE"`) |
| Speed limit 50 | no | 274 | 50 | 50 / km/h | §14.1 XML example (DE) |
| Pedestrian crossing (warning) | no | 101 | 11 | — | DE catalog |

These three rows are the *authored* palette. Any other `type`/`subtype` parses
into the **Modeled** tier (typed fields populated, no palette entry) and
round-trips; the editor simply cannot place it from the library panel.

## 4. Validation rules

New rule-id constants in `xodr/rules.hpp` (descriptions quoted verbatim from the
ASAM Annex; the version component is first-appearance per the existing
convention):

```cpp
// Objects — §13
kObjectTypeAttr          = "asam.net:xodr:1.7.0:road.object.type_attr";
kObjectOrientation       = "asam.net:xodr:1.7.0:road.object.orientation";
kObjectStTCoords         = "asam.net:xodr:1.7.0:road.object.s_t_coords";
kObjectCircularVsAngular = "asam.net:xodr:1.7.0:road.object.circular_vs_angular";
kOutlineExactlyOneOuter  = "asam.net:xodr:1.9.0:road.object.outline.exactly_one_outer";
kOutlineFollowedByCorner = "asam.net:xodr:1.9.0:road.object.outline.outline_followed_by_corner";
kCornerRoadMinAmount     = "asam.net:xodr:1.7.0:road.corner_road.element_min_amount";
kCornerRoadLocalExcl     = "asam.net:xodr:1.9.0:road.corner_road.corner_road_local_exclusivity";
// Signals — §14
kSignalType              = "asam.net:xodr:1.7.0:road.signal.signal_type";
kSignalUseCountryCode    = "asam.net:xodr:1.7.0:road.signal.use_country_code";
```

`validate_network` gains an object/signal pass that checks, for every parsed
element:

1. **Type present** — object `@type` and signal `@type`/`@subtype` set
   (`kObjectTypeAttr`, `kSignalType`); missing → `Warning` with the rule ID
   (never a drop).
2. **Shape exclusivity** — an object uses length/width **xor** radius
   (`kObjectCircularVsAngular`); both present → `Warning`.
3. **Origin present** — required `s`/`t`/`zOffset` (`kObjectStTCoords`); the
   parser defaults missing-but-required to 0 and warns.
4. **Outline well-formed** — exactly one `@outer=true` per object
   (`kOutlineExactlyOneOuter`); an `<outline>` has ≥2 `<cornerRoad>` or ≥2
   `<cornerLocal>`, never mixed (`kCornerRoadMinAmount`, `kCornerRoadLocalExcl`).
5. **Country code** — signal `@country` present (`kSignalUseCountryCode`);
   authored GS-1 signals always set it, foreign signals lacking it warn.
6. **ID uniqueness** — object/signal `@id` unique within its class across the
   file, reusing the existing `kIdUniqueInClass` machinery.

Every diagnostic carries `rule_id`; interpretation conflicts resolve to the
local reference text (CLAUDE.md standards rule).

## 5. Round-trip fidelity — never drop

The **Preserved** tier is how "the parser never silently drops input" is honored
for the large parts of §13/§14 M3a does not model:

- `xodr/raw_xml.hpp` defines `RawXml` — an owned copy of an element's *unknown*
  attributes and *unmodeled* child elements (e.g. `<skeleton>`, `<curveLocal>`,
  signal `<validity>`, `<dependency>`, `<reference>`, `<userData>`), captured at
  parse time in document order.
- The writer re-emits modeled fields from the typed struct, then appends the
  preserved fragment, producing a byte-stable round-trip for foreign files
  within the writer's normalization (attribute order is normalized; content is
  not lost). This is the same guarantee M2's writer already provides for roads.
- Round-trip tests (§ test plan) assert: parse → write → parse yields equal
  typed fields **and** equal preserved fragments; a fixtures corpus of foreign
  files (incl. the ASAM `Ex_TrafficIsland-CornerRoad`, `object_skeleton_pole`,
  and a signals example) must survive unchanged.

## 6. Version handling

The writer targets the version chosen by the M2 version-explicit writer:

- `<object>`/`<signal>`/`<repeat>`/`<outline>` core attributes exist since ≤1.4
  — always emitted.
- `@perpToRoad` (object) and signal `@length` are **1.8.0** — emitted only when
  target ≥1.8.0.
- `@temporary`/`@invalidated` and `<curveLocal>` are **1.9.0** — emitted only
  when target ≥1.9.0; below that they live only in the preserved fragment for
  foreign 1.9 files down-targeted, with a code comment citing both chapters.
- `country="OpenDRIVE"` catalog signals (traffic lights) require the Signal
  reference; emitted as-is (the value is version-neutral).

## 7. Normative reading (mandated before implementation)

Read for this design (CLAUDE.md "read the chapter first" rule):

- **§13 Objects** — `<object>` attribute table (Table 85): `id`, `s`/`t`/
  `zOffset` required, `type`/`subtype`, `orientation`, angular-vs-circular
  bounding volume, `hdg`/`pitch`/`roll`, `perpToRoad` (1.7.0),
  `temporary`/`invalidated` (1.9.0). `<outline>`/`<cornerRoad>`/`<cornerLocal>`
  (Tables 86–88) for crosswalks and painted markings; `<skeleton>`/`<polyline>`
  (Tables 92–94, preserved not authored); `<repeat>` (§13.4) for tree lines.
  Object markings (§13.8) feed [`02`](02_road_marks.md).
- **§14 Signals** — `<signal>` attribute table (Table 122): `id`/`s`/`t`/
  `zOffset`/`dynamic`/`orientation`/`type`/`subtype` required; `country`/
  `countryRevision`, `value`/`unit`, `hOffset`, `height`/`width`/`length`
  (1.8.0). Orientation/`hOffset` facing semantics (§14.1). Catalog signals with
  `country="OpenDRIVE"` for traffic lights. `<validity>`/`<dependency>`/
  `<controller>` links are preserved, not authored (M4).

Key interpretation decisions recorded here so implementation doesn't
re-derive them: (a) crosswalks/arrows/stop-lines are **objects** (§13.1
explicitly: "Specific road markings for control and regulation … are instead
represented as signals" — but crosswalks and painted arrows are *not* control
signals; they are object markings per §13.8), only signs/lights are signals;
(b) objects never move or re-orient (§13.1), so `MoveObject` edits placement
attributes, not a motion model; (c) the bounding volume is representation-only
in M3a — meshing uses the prop asset, not the box (`04` §3).

## 8. Test plan

- **Unit (kernel, GoogleTest):** construct each authored object/signal type via
  the arena API; assert field defaults, shape exclusivity, orientation.
- **Parse/write round-trip:** the foreign-fixtures corpus (§5) parses → writes →
  re-parses byte-stable; typed fields and preserved fragments equal.
- **Validation:** malformed fixtures (missing `@type`, both radius+length, two
  outer outlines, mixed corner kinds, missing `@country`) each produce exactly
  the expected `rule_id`; well-formed GS-1 elements produce none.
- **Fuzz:** add `<object>`/`<signal>`/`<repeat>`/`<outline>` samples to
  `core/tests/fuzz/corpus/` (CLAUDE.md rule for new xodr features).
- **Python (pytest):** bindings expose `add_object`/`add_signal` and iteration;
  one `python/examples/` script authors a signal + crosswalk and writes valid
  `.xodr` (same-PR binding rule).
- **Sanitizer:** ASan+UBSan over the parse/write path (macOS: no
  `detect_leaks`, per the known gotcha) before merge.

## 9. As-built — Phase 0 (`<objects>`, PR #74)

Phase 0 shipped per this design with the following deviations. **Phase 1
(`<signals>`, #68) and phase 2 (#69) must follow the as-built patterns below,
not the §2 sketches, wherever the two differ.**

- **Optionality is round-trip-driven.** Attributes that are optional in the
  schema stay `std::optional<>` so an absent attribute is never invented on
  write: `Object::height`, `valid_length`, `dynamic`, and
  `ObjectOutline::closed` (absent `@closed` has a type-dependent default per
  §13.14, so absence must survive). The §2 sketches showed plain doubles/bools
  for some of these. Apply the same rule to `Signal` (e.g. `height`/`width`/
  `length` already optional in §2.3).
- **Original spellings survive enums:** `Object::type_str` keeps `@type`
  exactly as spelled in the file; the enum is a classification, the string is
  authoritative on write. A signal's `type`/`subtype` are already free strings
  (§2.3), so no `type_str` twin is needed there.
- **`RawXml`** (`xodr/raw_xml.hpp`) is `attributes` (name/value pairs, document
  order) + `children` (serialized fragments) with an `empty()` helper — not an
  opaque fragment string as §5 loosely sketched.
- **Outline fallback:** an `<outline>` that cannot be modeled faithfully
  (`<curveLocal>` children or mixed corner kinds) is preserved whole in
  `ObjectOutline::raw` with `corners` left empty; the writer re-emits it
  verbatim inside `<outlines>`.
- **`<repeat>` is 0..*:** `Object::repeats` is a vector, not the single
  `optional<ObjectRepeat>` of §2.2; `ObjectRepeat` gained the 1.9.0 cubic
  `b_t`/`c_t`/`d_t` and the 1.8.0 `detach_from_reference_line`.
- **`ObjectOutline::lane_type`** is `optional<std::string>` (not `LaneType` —
  the outline enum is wider than the lane enum and must round-trip unknown
  values).
- **Road-level container extras:** non-`<object>` children of `<objects>`
  (`<objectReference>`, `<tunnel>`, `<bridge>`, §13.10–§13.12) are preserved
  as verbatim fragments in `Road::object_extras`. Phase 1 mirrors this with
  `Road::signal_extras` for non-`<signal>` children of `<signals>`
  (`<signalReference>`, §14.5 — multiplicity 0..*).
- **Legacy acceptance:** the 1.4-era direct-child `<outline>` (no `<outlines>`
  wrapper) parses; the writer always emits the wrapped form.
- **Not shipped in Phase 0 (deliberate, see §2.4 phase ownership):** edit
  commands, `DirtySet::objects`, and any mesh work.
</content>
