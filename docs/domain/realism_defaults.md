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
| Crosswalk width | **min 1.8 m** | 6 ft |
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
| Lateral offset | min **1.8 m** from shoulder edge; urban min **0.6 m** from curb face | 6 ft; 2 ft | interacts with #338's outermost-lane-edge soft-snap — placement must compose with it |
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
| Street light | mounting height **9.0 m**; residential 7.6 m, arterial up to 12 m | 30 ft; 25 ft; 40 ft |
| Fire hydrant | 0.75 m | 30 in |

## 1.6 Trees & buildings

<!-- rm-defaults: trees-buildings -->
| Item | Default | Range |
|---|---|---|
| Street tree (default asset) | **height 10 m**, canopy Ø ≈ 6 m, trunk Ø 0.4 m; clear trunk ≥ 2.4 m over sidewalk / 4.4 m over roadway | small ornamental 4–6 m; large mature 15–20 m |
| House, 1-story | **5 m** to ridge | 4–6 m |
| House, 2-story | **8 m** | 7–9 m |
| Commercial 1-story | **5.5 m** | 4.5–6 m |
| Mid-rise | **3.7 m per floor** + 1 m parapet | residential floors 3.0 m |
| Building footprint sanity | a house is not smaller than 2 car lengths per side (≈ 10 × 8 m typical) | — |

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
[#415](https://github.com/Robomous/RoadMaker/issues/415) (props) and
[#414](https://github.com/Robomous/RoadMaker/issues/414) (signs). #413 may
regularize table formatting for the comparator; if it does, these tables
are regenerated from the registry in that PR.

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

### Markings (→ #413)

| Item | Old (code) | Where | New |
|---|---|---|---|
| Line width (`RoadMark`/`RoadMarkLine`) | 0.12 m | `core/include/roadmaker/road/lane.hpp` | §1.3 |
| Broken-line pattern | 3.0 m dash / 6.0 m gap | `core/src/mesh/mesh_builder.cpp` | §1.3 |
| Double-line separation | one mark-width (0.12 m) | `core/src/mesh/mesh_builder.cpp` | §1.3 |
| Stop line | 0.30 m | `core/include/roadmaker/mesh/junction_stoplines.hpp`, manifest | §1.3 |
| Crosswalk | depth 3.0 m, zebra 0.5/0.5 | `core/include/roadmaker/edit/markings.hpp`, manifest | compliant with §1.3 (≥ 1.8 m); stripe pattern re-derived per MUTCD longitudinal style with #413 |

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
| Orientation on placement | always `+`, hOffset 0, no side logic | `signal_placement.cpp` | auto-orientation section |

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

### Interaction defaults (→ #416/#417)

| Item | Old | New |
|---|---|---|
| Sign/signal facing | none (always along +s) | auto-orientation section |
| Prop Z-rotation snapping | none (free ring) | 15° + suppression modifier (orientation table) |
