# Realism defaults (North American baseline)

*The canonical spec for RoadMaker's default dimensions, proportions, unit
policy, sign-pack content, and orientation rules. This is the **only** place
these defaults are defined: issues and code comments reference this document
and never restate its numbers; any change to a default is a PR against this
document first. Enforcement is structural — see
[Enforcement](#enforcement-machine-readable-table--ci) below. Batch tracking:
[#411](https://github.com/Robomous/RoadMaker/issues/411); roadmap record:
[2026-07 realism batch](../roadmap/updates/2026-07-realism-batch.md).*

## Baseline and sources

Defaults follow **North American practice**: AASHTO's geometric-design
policy (the Green Book) for cross sections, the MUTCD (11th edition) for
traffic-control devices, with ADA/NACTO-typical values where those bodies
govern. All values here are re-expressed from **publicly available
standards literature** — never from any commercial product — per the
[product-parity rules](../standards/product-parity.md).

**Metric is canonical.** Persistence is SI meters unconditionally (ASAM
OpenDRIVE is metric); the imperial column is the *display* equivalent used
by the units toggle
([#412](https://github.com/Robomous/RoadMaker/issues/412)). Where a US
standard is written in feet/inches, the metric canonical value is the
practice-standard soft conversion (12 ft → 3.6 m), not the exact hard
conversion.

## Unit policy

- The kernel, the persistence layer, and every command capture value are
  **SI meters and radians, always**. No imperial value is ever stored.
- The editor's metric/imperial toggle (#412) is a display + input-parsing
  layer only: display formats in the active system; input accepts the
  active unit and commits meters. Metric is the default.
- Imperial display uses ft/in for dimensions and **mph** where speeds
  appear.
- **Sign faces are not readouts.** A US-format speed-limit face renders its
  value in mph regardless of the UI unit setting — it is a depiction of a
  physical sign.

## 1.1 Reference vehicle (the proportional anchor)

<!-- rm-defaults: reference-vehicle -->
| Item | Value |
|---|---|
| Passenger car (AASHTO P design vehicle) | width 2.13 m (7 ft), length 5.79 m (19 ft), height ≈ 1.45 m |
| Typical actual passenger car | ≈ 1.85 m wide — a 3.6 m lane gives ~0.85 m clearance each side |
| Legal max truck height (bridge-clearance driver) | 4.11 m (13 ft 6 in); interstate min vertical clearance 4.9 m (16 ft) |

Every default below must read correctly against this anchor: a lane fits a
car with sensible clearance, a tree clears a truck, a sign face is legible
at driving distance.

## 1.2 Lane & cross-section defaults (per road class)

<!-- rm-defaults: cross-section -->
| Element | Default | Imperial display | Range | Basis |
|---|---|---|---|---|
| Freeway/highway lane | **3.6 m** | 12 ft | 3.6 | AASHTO |
| Arterial lane | **3.6 m** | 12 ft | 3.3–3.6 | AASHTO |
| Collector lane | **3.3 m** | 11 ft | 3.0–3.6 | AASHTO |
| Local/residential lane | **3.0 m** | 10 ft | 2.7–3.6 | AASHTO |
| Freeway right shoulder | **3.0 m** | 10 ft | — | AASHTO |
| Freeway left shoulder | **1.2 m** | 4 ft | — | AASHTO |
| Arterial/collector shoulder | **1.8 m** | 6 ft | 0.6–2.4 | AASHTO |
| Parking lane | **2.4 m** | 8 ft | 2.4–2.7 | typical NA |
| Bike lane | **1.5 m** | 5 ft | 1.5–1.8 | AASHTO/NACTO |
| Sidewalk | **1.8 m** | 6 ft | 1.5 min | ADA/typical |
| Curb height | **0.15 m** | 6 in | 0.10–0.20 | typical NA |
| Raised median | **1.2 m** min | 4 ft | — | AASHTO |
| Two-way left-turn lane | **3.6 m** | 12 ft | 3.0–4.2 | AASHTO |

The four **road classes** (freeway / arterial / collector / local) are the
default authoring presets — both create-road templates and Library road
styles derive from this table
([#413](https://github.com/Robomous/RoadMaker/issues/413), extending
p2-s8's road-style assets). The junction sidewalk band inherits the
sidewalk lane width by construction, so it follows this table
automatically.

*Curb height note:* curbs are not vertically extruded by the current mesh
pipeline; the value above is canonical for when they are, and for any
feature (e.g. clearance checks) that needs the number meanwhile.

## 1.3 Markings (MUTCD Ch. 3)

<!-- rm-defaults: markings -->
| Item | Default | Imperial display |
|---|---|---|
| Normal line width | **0.10 m**; freeway option 0.15 m | 4 in; 6 in |
| Broken lane line | **3.0 m dash / 9.0 m gap** | 10 ft / 30 ft |
| Double yellow centerline | two normal lines, **0.10 m** apart | 4 in |
| Stop line | **0.60 m** wide; min 0.30 m | 24 in; 12 in |
| Crosswalk transverse lines | 0.15–0.60 m wide | 6–24 in |
| Crosswalk zebra stripe | **0.60 m bar / 0.60 m gap** | 24 in / 24 in |
| Crosswalk width | **3.0 m** walking depth; min 1.8 m | 10 ft; 6 ft |
| Edge lines | white right edge; yellow left edge (divided) | — |

## 1.4 Signs (MUTCD, conventional-road sizes)

Face sizes are authored geometry; symbols and legends are baked textures
([#414](https://github.com/Robomous/RoadMaker/issues/414)).

<!-- rm-defaults: signs -->
| Sign | Face size (default) | Imperial display | Notes |
|---|---|---|---|
| Stop (R1-1) | **0.75 × 0.75 m** octagon | 30 in | 0.90 m multilane option |
| Yield (R1-2) | **0.90 m** triangle | 36 in | |
| Speed Limit (R2-1) | **0.60 × 0.75 m** | 24 × 30 in | value editable; the face displays **mph** regardless of UI units |
| Do Not Enter (R5-1) | 0.75 × 0.75 m | 30 in | |
| One Way (R6-1) | 0.90 × 0.30 m | 36 × 12 in | arrow direction = variant |
| Turn restriction (R3-1/R3-2) | 0.60 × 0.60 m | 24 in | symbol |
| Keep Right (R4-7) | 0.60 × 0.75 m | 24 × 30 in | |
| Warning diamonds (W1-2 curve, W3-1 stop ahead, W11-2 pedestrian) | **0.75 × 0.75 m** | 30 in | |
| School (S1-1) | 0.90 m pentagon | 36 in | |
| Street name (D3-1) | 0.25–0.30 m tall, length fits text | 10–12 in | text editable; letter height ≥ 0.15 m |

<!-- rm-defaults: sign-mounting -->
| Mounting | Default | Imperial display | Notes |
|---|---|---|---|
| Mounting height (bottom edge above pavement) | **urban 2.1 m**; rural option 1.5 m | 7 ft; 5 ft | |
| Lateral offset | min **1.8 m** from shoulder edge; urban min **0.60 m** from curb face | 6 ft; 2 ft | interacts with #338's outermost-lane-edge soft-snap — placement must compose with it |
| Post | breakaway single post, visual Ø ≈ 0.06 m | — | |

### Sign definitions are data

A sign's shape, size, colors, and legend slots are **data, not code**: the
US pack is the first data set, and a future country pack is a new data set
with **no engine change**. Symbol artwork is authored in-repo as SVG after
the public-domain US federal sign specifications (no third-party artwork
files); legends render at texture-build time with a bundled SIL-OFL
highway-style typeface (fonts-as-assets license exception, maintainer
approved; the license file ships with the font per the
[asset policy](../standards/assets.md)). The pack *system* (selection UI,
multiple packs) is deliberately not built yet.

### Persistence

OpenDRIVE `<signal>` with `country="US"`, `type` = the sign designation
(e.g. `R1-1`), `dynamic="no"` for static signs / `"yes"` for signal heads,
`height`/`width` from the tables above, and `value` + `unit` for speed
limits.

## 1.5 Signals, lighting, street furniture

<!-- rm-defaults: signals-lighting -->
| Item | Default | Imperial display |
|---|---|---|
| Signal head | 3-section, **0.30 m** lenses, housing ≈ 1.07 m tall | 12 in |
| Signal vertical clearance | bottom of housing **4.6–5.8 m** over roadway; default **5.2 m** with mast arm | 15–19 ft; 17 ft |
| Post-mounted / pedestrian signal | mounting 2.1–3.0 m | 7–10 ft |
| Street light | mounting height **9.0 m**; residential 7.6 m, arterial up to 12.0 m | 30 ft; 25 ft; 40 ft |
| Fire hydrant | 0.75 m | 30 in |

## 1.6 Trees & buildings

<!-- rm-defaults: trees-buildings -->
| Item | Default | Range |
|---|---|---|
| Street tree (default asset) | **height 10.0 m**, canopy Ø ≈ 6.0 m, trunk Ø 0.40 m; clear trunk ≥ 2.4 m over sidewalk / 4.4 m over roadway | small ornamental 4.0–6.0 m; large mature 15.0–20.0 m |
| House, 1-story | **5.0 m** to ridge | 4.0–6.0 m |
| House, 2-story | **8.0 m** | 7.0–9.0 m |
| Commercial 1-story | **5.5 m** | 4.5–6.0 m |
| Mid-rise | **3.7 m per floor** + 1.0 m parapet | residential floors 3.0 m |
| Building footprint sanity | a house is not smaller than 2 car lengths per side (≈ 10.0 × 8.0 m typical) | — |

## 1.7 Road type (per road class)

([#454](https://github.com/Robomous/RoadMaker/issues/454)) OpenDRIVE
`<type>` (§10.4) is the standard's own carrier for the concept §1.2's
road classes describe, so the two are bound here rather than in each
consumer. The `e_roadType` enumeration is **identical in 1.8.1 and
1.9.0** — 13 literals, same order — so nothing about this table is
revision-conditional.

<!-- rm-defaults: road-type -->
| Road class | OpenDRIVE `@type` |
|---|---|
| freeway | `motorway` |
| arterial | `townArterial` |
| collector | `townCollector` |
| local | `townLocal` |

Authoring applies this binding: a road created from one of the four
templates (`LaneProfile::freeway()` and friends) is stamped with its
class's `<type>` at `s = 0`, and `edit::apply_road_style` rewrites that
`@type` when a class-bearing style is applied. A profile or style
assembled by hand carries no class and stamps nothing — a bespoke cross
section is not entitled to claim it is a `motorway`.

Because the record carries more than the class, a restyle **keeps** the
old record's `@country`, its `<speed>` and any preserved extras and
changes only `@type`. That follows directly from the paragraph below: if
no class supplies a speed, no class may take one away either.

**There is deliberately no per-class default speed.** A speed limit is a
fact about a particular road, not about its class, and `<speed>` is
optional (multiplicity 0..1) precisely so a file can decline to claim
one. Inventing 35 mph for every collector whose source recorded no limit
would be guessing where the honest answer is silence — the rule
[ADR-0010](../decisions/0010-gis-ingest-bounded-crs.md) states for
coordinate systems, applied to speeds. A road's `<speed>` is written only
when something actually knows it: an imported `maxspeed`, or a user.

`@max` is `t_maxSpeed`, a **union of a number and the two string literals
`no limit` and `undefined`** — which is why the data model keeps the
verbatim spelling and derives the number from it, never the reverse.

## 1.8 Scenario actors

([#246](https://github.com/Robomous/RoadMaker/issues/246)) The archetypes a
scenario actor can be placed from, in toolbar order. Unlike every other table
here these are **OpenSCENARIO** entities, not OpenDRIVE content — they reach
`<Entities><ScenarioObject>` in the `.xosc`, never the `.xodr` — so the table is
rendered by `osc::actor_catalog_markdown()` rather than by
`roadmaker::defaults`: `defaults` must not depend on `osc`, since every
dependency in this tree runs `osc` → `road` and never back.

Vehicle dimensions are the **AASHTO design vehicles** — P (passenger car), SU-30
(single-unit truck), BUS-40 (city transit bus) and B (bicycle) — which is the
same source §1.1 anchors every other default to. The **Car row is §1.1's
reference vehicle exactly**, and a test pins it there: it is the proportional
anchor lane widths and clearances are measured against, so an actor that
disagreed with it would make all of them read wrong.

<!-- rm-defaults: actors -->
| Actor | Category | Width | Length | Height | Mass |
|---|---|---|---|---|---|
| Car | `car` | 2.13 m | 5.79 m | 1.45 m | 1500 kg |
| Truck | `truck` | 2.44 m | 9.14 m | 4.11 m | 12000 kg |
| Bus | `bus` | 2.59 m | 12.19 m | 3.20 m | 15000 kg |
| Motorbike | `motorbike` | 0.80 m | 2.20 m | 1.50 m | 250 kg |
| Bicycle | `bicycle` | 0.60 m | 1.80 m | 1.70 m | 100 kg |
| Pedestrian | `pedestrian` | 0.60 m | 0.40 m | 1.75 m | 80 kg |

The truck's 4.11 m height is §1.1's legal maximum (13 ft 6 in), deliberately:
the tallest legal vehicle is what every overhead clearance in this document is
sized to clear, so the placeable truck is the one that tests those clearances.

`<Performance>` and `<Axles>` are **not** tabulated. They are required children
of `<Vehicle>` in every OpenSCENARIO revision and they follow from the
dimensions above (wheelbase, wheel diameter, top speed); the values live in
`core/src/osc/catalog.cpp` beside the row they belong to, and a test asserts
every archetype builds a `<ScenarioObject>` that `write_xosc` accepts as-is.

## Auto-orientation of signs & signals

([#416](https://github.com/Robomous/RoadMaker/issues/416)) The default
facing on placement is computed from the road heading at s, the side (sign
of t), and the travel direction of the nearest driving lane
(left-of-reference lanes run against +s in right-hand traffic). The face
normal points **against** approaching travel, with a standard **toe-out of
3° (0.052 rad)** from perpendicular to reduce headlight glare.

<!-- rm-defaults: orientation -->
| Constant | Value |
|---|---|
| Sign/signal toe-out from perpendicular | **3° (0.052 rad)** |
| Prop rotation-ring snap increment | **15°** |

Persistence is the OpenDRIVE signal `orientation` (+/−) plus a heading
offset. **A user-set heading is an override: it is never re-auto-computed
silently** — re-auto happens only through the explicit "auto" action. This
rule binds every later feature that relocates or recomputes placements
(including the cascade epic
[#406](https://github.com/Robomous/RoadMaker/issues/406)).

## Enforcement (machine-readable table + CI)

The values in the `rm-defaults`-marked tables above are mirrored by **one
machine-readable code table** from which the authoring templates, road
styles, marking constants, and prop/sign dimensions derive. A CI test
asserts this document's marked tables match the code table exactly — the
same mechanism that keeps `docs/user-guide/shortcuts.md` honest against
`shortcut_registry` (`editor/tests/test_shortcut_registry.cpp`).
Divergence fails CI, not review. The registry and test land with
[#413](https://github.com/Robomous/RoadMaker/issues/413) (cross-section +
markings) and are extended by
[#415](https://github.com/Robomous/RoadMaker/issues/415) (props),
[#414](https://github.com/Robomous/RoadMaker/issues/414) (signs) and
[#416](https://github.com/Robomous/RoadMaker/issues/416) (auto-orientation —
the registry's first ANGLES, stored in radians and constructed from their
degree measure) and
[#417](https://github.com/Robomous/RoadMaker/issues/417) (which bound the
viewport's rotation ring to the orientation table's increment through a test,
not merely through the compiler). #413 may
regularize table formatting for the comparator; if it does, these tables
are regenerated from the registry in that PR.

Props are the one consumer that cannot *include* the code table:
`scripts/gen_prop_meshes.py` is stdlib-only so it runs on a bare CI runner.
It therefore bakes the §1.5/§1.6 targets into the meshes, and the same test
asserts the bundled model dimensions against the registry — the derivation
is enforced rather than compiled, but it fails CI just the same.

## Changelog — old → new (audited at `main` @ e18592b, 2026-07-24)

Recorded once, here, so no issue or commit restates values. "New" is the
governing table above; dispositions land with the implementing PRs.

### Cross section (→ #413)

| Item | Old (code) | Where | New |
|---|---|---|---|
| Driving lane (rural/urban templates, `LaneSpec`/`StyleLane` fallback, add-lane fallback) | 3.5 m | `core/src/road/authoring.cpp`, `core/include/roadmaker/road/road_style.hpp`, `core/src/edit/operations.cpp` | per class, §1.2 |
| Driving lane (highway template/style) | 3.75 m | `core/src/road/authoring.cpp`, `core/src/road/road_style.cpp` | freeway class, §1.2 |
| Shoulder (rural template) | 1.0 m | `core/src/road/authoring.cpp` | arterial/collector shoulder, §1.2 |
| Shoulder (highway template/style) | 2.5 m | `core/src/road/authoring.cpp` | freeway right/left shoulders, §1.2 |
| Sidewalk (urban template) | 2.0 m | `core/src/road/authoring.cpp` | §1.2 (junction bands follow by construction) |
| Curb height | not modeled | — | §1.2 (canonical value; extrusion out of scope) |
| Parking / bike / median / TWLTL widths | no defaults existed | — | §1.2 |
| Road classes | 3 templates (rural / urban / highway); 1 shipped style | `authoring.cpp`, `road_style.cpp`, `assets/library/manifest.json` | 4 classes, §1.2, templates + styles from one table |

*Dispositions (landed with #413):* the registry is
`core/include/roadmaker/road/defaults.hpp` (`roadmaker::defaults`); the doc
tables above are rendered by its `cross_section_markdown()` /
`markings_markdown()` and gated by `core/tests/test_defaults_registry.cpp`.
The legacy template/style names remain as aliases of their nearest class —
`two_lane_rural`/`two_lane_default` → collector, `urban_sidewalk` → local,
`highway` → freeway, `urban_two_lane` → arterial — and the Library now ships
the four class templates and styles. The Create Road tool's default template
is the local street (successor of the old urban-sidewalk default). Classless
add-lane/taper paths use the per-lane-type column of §1.2, with the arterial
driving lane as the fallback for unlisted types.

### Markings (→ #413)

| Item | Old (code) | Where | New |
|---|---|---|---|
| Line width (`RoadMark`/`RoadMarkLine`) | 0.12 m | `core/include/roadmaker/road/lane.hpp` | §1.3 |
| Broken-line pattern | 3.0 m dash / 6.0 m gap | `core/src/mesh/mesh_builder.cpp` | §1.3 |
| Double-line separation | one mark-width (0.12 m) | `core/src/mesh/mesh_builder.cpp` | §1.3 |
| Stop line | 0.30 m | `core/include/roadmaker/mesh/junction_stoplines.hpp`, manifest | §1.3 |
| Crosswalk | depth 3.0 m, zebra 0.5/0.5 | `core/include/roadmaker/edit/markings.hpp`, manifest | §1.3: depth 3.0 m kept (≥ 1.8 m min), zebra re-derived to 0.60/0.60 (MUTCD longitudinal style) |

### Signs & signals (→ #414)

| Item | Old (code) | Where | New |
|---|---|---|---|
| Sign identities | German StVO set: types 206 (stop), 205 (yield), 310 (text), 274/50 (speed), `country="DE"` | `editor/src/document/signal_placement.cpp` | US pack, §1.4 |
| Stop sign face | octagon ≈ 0.84 m across, plate center 2.35 m | `scripts/gen_prop_meshes.py` | §1.4 |
| Yield face | triangle ≈ 1.0 m | `scripts/gen_prop_meshes.py` | §1.4 |
| Generic sign face | 0.52 m disc (0.64 m rim) | `scripts/gen_prop_meshes.py` | replaced by designated §1.4 set |
| Text plate | 1.10 × 0.66 m | `scripts/gen_prop_meshes.py` | street name (D3-1), §1.4 |
| Sign post | Ø 0.10 m, 2.2 m pole | `scripts/gen_prop_meshes.py` | §1.4 mounting table |
| Speed-limit value | baked into StVO `subtype` string, km/h, not editable | `signal_placement.cpp` | `value`+`unit` (mph face), editable, §1.4 |
| Signal head | housing 0.84 m, lenses 0.14 m, pole 3.0 m | `scripts/gen_prop_meshes.py` | §1.5 |
| Signal mounting | hand-placed zOffset 0; template heads 3.0 m / plates 2.2 m | `signal_placement.cpp`, `core/src/edit/operations.cpp` | §1.5 clearances / §1.4 mounting |
| Orientation on placement | always `+`, hOffset 0, no side logic; the renderer ignored `@orientation` | `signal_placement.cpp`, `core/src/mesh/mesh_builder.cpp` | auto-orientation section |

*Dispositions (landed with #414, identities first):* the three hard-coded
identity tables — `make_signal`'s tag chain, the mesh builder's
`signal_model_id`, and the junction signalize templates' `SignalCode`s — were
replaced by **one data table**, `roadmaker::signs::catalog()`
(`core/include/roadmaker/assets/sign_catalog.hpp`), whose every face extent is a
`roadmaker::defaults` §1.4 constant. `signs_markdown()` /
`sign_mounting_markdown()` render the two tables above and
`core/tests/test_defaults_registry.cpp` gates both the doc text and the
catalogue, with `editor/tests/test_library_model.cpp` covering the manifest half
(core cannot read the Qt-JSON manifest). Per item:

- **Identities** — the pack ships as `country="US"` with the MUTCD designation
  as `@type` (§14.1 defines `@type` as a "type identifier according to country
  code" and `@country` as an ISO 3166-1 alpha-2 code, so no vendor extension is
  needed). The Library lists one entry per designation, and its tag **is** the
  catalogue key. The traffic-light head keeps the country-neutral ASAM
  catalogue code `1000001`/`OpenDRIVE`.
- **Speed-limit value** — now `@value` + `@unit="mph"` (an `e_unitSpeed`
  literal, Table 158), editable through a new `edit::set_signal_value` command
  and a **Speed limit** row in the Attributes pane. The face renders the value
  in mph regardless of the display-unit toggle, because the kernel derives the
  legend from `@value` and never consults a UI setting. A `@value` without its
  `@unit` now raises a validator advisory (ASAM lists no rule id for the
  pairing, so the finding cites none).
- **Face sizes** — a placement writes `@height`/`@width` from §1.4. A D3-1
  street-name blade declares no `@width`: its length follows its legend.
- **Legacy German identities degrade, they do not break.** A pre-#414 scene
  still parses, re-exports byte-identically, and renders on the generic
  silhouette; the German fuzz-corpus seeds stay German on purpose as the
  regression for it. The generic-drop default moved off the StVO catalog to the
  pack's stop sign.
- **`assets/samples/gs1_urban_intersection.xodr`** was regenerated from
  `python/examples/build_gs1.py`, which also picked up the §1.2 cross-section
  and crosswalk-outline drift that had accumulated since it was last written.
- **Meshes and artwork (landed second).** Every pack sign is built by a data
  table in `scripts/gen_prop_meshes.py` — silhouette, §1.4 face size, Ø 0.06 m
  post, face bottom edge at the 2.1 m mounting height — and
  `test_defaults_registry.cpp` asserts all of it against the registry, the same
  enforced-not-compiled arrangement the §1.5/§1.6 props use. The face size is
  the OVERALL plate, so the border is an inset ring rather than an addition.
- **Faces are three composited layers**: the flat field, the sign's artwork,
  and up to two text layers (the sign's *fixed* legend and the placed signal's
  *editable* one), each in its own normalised box so a speed-limit face can
  carry its wordmark above and its posted number below. Artwork is authored
  in-repo as SVG in `assets/signs/us/` after the public-domain US federal sign
  specifications — no third-party artwork file — and the kernel rasterises it
  with **nanosvg** (zlib), because `core/` must never link Qt and faces are
  baked headless by the glTF exporter and from Python. nanosvg parses shapes
  only, which is why every legend is a text layer rather than outlined
  lettering, and why they all share one typeface.
- **The typeface is Overpass** (OFL-1.1), the open highway-gothic alike, and it
  replaced Roboto — `render_face` was Roboto's only consumer. The OFL text
  ships beside the font; upstream is dual OFL/LGPL and RoadMaker takes the OFL
  half only, so Qt remains the project's single LGPL dependency.
- **A face is attached only when there is something to draw** — artwork, a
  fixed legend, or an editable one. The old gate was "non-empty `@text`", which
  a symbol sign fails by construction; the new one also means a blank plate
  costs no texture. A legacy sign's `@text` now draws on the fallback
  silhouette instead of being silently dropped.
- **`assets/samples/sign_pack.xodr`** (built by `scripts/make_canonical_scenes.py`,
  rendered by CI's visual-artifacts job) is the standing visual check: every
  pack sign along a local street, against the same ruler `props_scale.xodr`
  uses.

### Props (→ #415)

Effective default = intrinsic model size × manifest `default_scale`
(`scripts/gen_prop_meshes.py` × `assets/library/manifest.json`).

| Asset | Old effective default | New |
|---|---|---|
| Pine tree | 8.4 m (4.2 × 2.0) | §1.6 street-tree band — near-compliant; fine-tune with #415 |
| Oak tree | 9.2 m (4.6 × 2.0) | §1.6 — near-compliant |
| Birch tree | 9.4 m (4.7 × 2.0) | §1.6 — near-compliant |
| Poplar tree | 12.0 m (6.0 × 2.0) | §1.6 — within large-tree range |
| Shrub | 2.4 m (1.2 × 2.0) | §1.6 ornamental band |
| Streetlight (single/double) | 5.5 m | §1.5 |
| Low building | 7.5 m, footprint 10.4 × 8.4 m | §1.6 2-story house band; footprint compliant |
| Mid-rise building | 20.3 m | §1.6 per-floor rule (≈ 5 floors + parapet) — verify with #415 |
| Tower building | 40.0 m | §1.6 per-floor rule — verify with #415 |
| Fire hydrant / street furniture | not shipped | recorded absent; additions only as follow-ups on #411 |

*Dispositions (landed with #415):* every bundled prop is now authored at its
**true world size** in `scripts/gen_prop_meshes.py`, and the plants'
`default_scale: 2.0` was retired from `assets/library/manifest.json` — the
scale mechanism itself stays (it is per-asset data), but nothing shipped uses
it, so a prop's declared model size is what a placement spawns. That is what
lets §1.5/§1.6 be gated in the kernel: `signals_lighting_markdown()` /
`trees_buildings_markdown()` render the tables above, and
`core/tests/test_defaults_registry.cpp` asserts both the doc text and
`props::model()` against the registry, with
`editor/tests/test_library_model.cpp` covering the manifest half (core cannot
read the Qt-JSON manifest). Per asset:

- **Oak** — retuned to *the* §1.6 default street tree: 10.0 m tall, canopy
  Ø 6.0 m, trunk Ø 0.40 m, crown starting at 4.4 m (the roadway clear-trunk
  rule, which also satisfies the 2.4 m sidewalk one).
- **Pine** (8.4 m), **birch** (9.4 m), **shrub** (2.4 m) — already compliant;
  their previous ×2 spawn size was baked into the meshes unchanged. The shrub
  stays deliberately below the 4 m small-ornamental minimum: it is not a tree.
- **Poplar** (12.0 m) — kept as authored. Above the 10 m default street tree
  and below the 15–20 m mature band, which is what a planted columnar poplar
  reads as; no retune.
- **Streetlight, single and double** — pole raised to the §1.5 mounting height
  of 9.0 m (from 5.5 m), pole Ø 0.30 m, arm reach 1.8 m with the lamp head
  hanging just below the top.
- **Low building** — already compliant, untouched: 7.5 m sits in the 7–9 m
  two-storey house band and the 10.4 × 8.4 m footprint clears the ≈ 10 × 8 m
  sanity check.
- **Mid-rise** — 20.3 m → **19.5 m**: five 3.7 m floors plus the 1 m parapet
  zone, which now contains the roof slab and the rooftop plant unit.
- **Tower** — 40.0 m → **38.0 m**: set-back stages of 5 + 4 + 1 floors of
  3.7 m, capped by a 1 m parapet.
- **Fire hydrant / street furniture** — still not shipped, recorded absent
  here; #415 added no new assets (that path stays a follow-up on #411).

Signal and sign *meshes* are untouched by #415 — the §1.5 signal rows are
rendered by the registry so the whole table is gated, but retuning the heads
themselves belongs to #414. `assets/samples/props_scale.xodr` (built by
`scripts/make_canonical_scenes.py`, rendered by CI's visual-artifacts job) is
the standing visual check: one of every prop at native size along a local
street.

### Interaction defaults (→ #416/#417)

| Item | Old | New |
|---|---|---|
| Sign/signal facing | none (always along +s) | auto-orientation section |
| Prop Z-rotation snapping | 15° on the drag *delta*, props only | absolute 15° from the road, props **and** signs (orientation table); suppression modifier in [Moving & transforming](../user-guide/moving-and-transforming.md) |

*(The "Old" column here was audited at `e18592b` as "none (free ring)". The
detent and its Shift suppression had in fact shipped with the gizmo in #188;
what #417 found missing was absolute snapping, sign/signal coverage, and any
test at all. Corrected with #417 rather than left standing.)*

The suppression modifier is deliberately **not** a row in the machine-gated
orientation table: it is a keyboard binding, and the kernel's defaults registry
does not name editor keys. The increment is the registry's; the key is the user
guide's.

*Dispositions (landed with #416):* placement and one explicit **Auto facing**
action now derive `@orientation` and `@hOffset` from the road, the side, and the
travel direction of the nearest driving lane — `roadmaker::auto_signal_facing`,
shared by hand placement and junction signalization (which previously
open-coded the direction and never set a heading at all). Those two call sites
are the ONLY places a facing is computed, which is how "a user-set heading is
never re-auto-computed silently" holds without a stored flag: nothing else can
recompute one.

The renderer was corrected in the same pass. It had ignored `@orientation`
entirely and aimed every signal along +s — the datum §14.1 defines for `"-"`,
applied to all three literals. Existing files with `orientation="+"` therefore
re-aim by 180°, which is the correction, not a regression.

*Dispositions (landed with #417):* the viewport's rotation ring now reaches
signs and signals, snaps props and signs to absolute increments, and is driven
from a headless-testable session rather than from inside the viewport widget.
A ring drag on a sign writes `@hOffset` and nothing else — `@orientation` is a
*validity* declaration under §14.1, so a gesture that turns a sign must not
change which traffic it applies to. The ring is therefore not one of the three
sites allowed to derive a facing, and a heading dragged there is an override on
exactly the same terms as one typed into the Attributes pane.

The sprint also fixed a selection bug: a sign carries its owning road, and the
gizmo resolved that road first, so **selecting a sign and dragging the ring
rotated the whole road**. Leaf entities now win, as they already did everywhere
else in the selection model.
