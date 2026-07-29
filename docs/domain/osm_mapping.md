# OpenStreetMap → OpenDRIVE mapping

*What an OSM way becomes when RoadMaker imports it, what is dropped, and what
every compromise is called in the diagnostics.*

This is a **governing spec document**: the tables marked `rm-osm` below are
mirrored by a code table in `core/include/roadmaker/osm/tags.hpp`, and
`core/tests/test_osm_mapping.cpp` fails CI when the two disagree. Divergence is
caught by a test, not by review — the same mechanism
[realism_defaults.md](./realism_defaults.md) uses for the defaults registry and
`docs/user-guide/shortcuts.md` uses for the shortcut registry.

Format scope and the reasoning behind it are
[ADR-0012](../decisions/0012-osm-ingest-xml-in-house.md): `.osm` XML is read
in-house with the already-linked pugixml, `.osm.pbf` is refused by name ([#494](https://github.com/Robomous/RoadMaker/issues/494)).

## The contract

**Nothing is dropped silently.** Every way that does not become a road, every tag
that is read and not used, every polyline that is simplified, and every junction
turn the generator could not fit produces a `Diagnostic` naming the OSM element
by id. That is the whole of what
[#244](https://github.com/Robomous/RoadMaker/issues/244) means by
*diagnostics-first fitting*, and it cuts both ways: **a way imported without
compromise produces no diagnostic at all.** Warning about everything is
indistinguishable from warning about nothing.

**No metre in this document is authored here.** The road classes name entries in
`roadmaker::defaults` ([realism_defaults.md](./realism_defaults.md) §1.2); every
lane width comes from that registry, and this mapping only ever selects a class
or adds and removes whole lanes. A width literal in the OSM importer would fail
`core/tests/test_defaults_registry.cpp`'s no-private-width-table gate, and
correctly so.

## 1. `highway=*` → road class

A way is imported only if its `highway` value appears in this table with a road
class. Everything else is dropped with a Warning quoting the value verbatim, so
an unmapped-but-real classification is visible rather than absent.

`*_link` values are ramps and slip roads: they take their parent's class but are
narrowed to a single driving lane per direction, because a ramp built to its
parent's full cross section is wrong everywhere.

<!-- rm-osm: highway -->
| `highway=` | Imported as | Link ramp | Implies oneway |
|---|---|---|---|
| `motorway` | freeway | no | yes |
| `motorway_link` | freeway | yes | yes |
| `trunk` | freeway | no | no |
| `trunk_link` | freeway | yes | yes |
| `primary` | arterial | no | no |
| `primary_link` | arterial | yes | yes |
| `secondary` | arterial | no | no |
| `secondary_link` | arterial | yes | yes |
| `tertiary` | collector | no | no |
| `tertiary_link` | collector | yes | yes |
| `unclassified` | collector | no | no |
| `residential` | local | no | no |
| `living_street` | local | no | no |
| `road` | collector | no | no |
| `service` | local | no | no |

Two rows need their reasoning stated, because both are judgement calls rather
than readings of the OSM wiki:

- **`road`** is OpenStreetMap's own "classification unknown" marker. It is
  imported as a collector — the middle of the four classes — and always emits an
  Info saying so, because the class is a guess and the user should know which
  roads it was guessed for.
- **`service`** covers everything from a signposted access road to a supermarket
  parking aisle, so it is **off by default** (`OsmBuildOptions::
  include_service_roads`). When enabled, `service=driveway` and
  `service=parking_aisle` are still dropped: at district scale they outnumber
  the actual road network and turn a legible import into a hairball.

### Dropped `highway` values

Recorded here so the list is reviewable, and asserted against the code table by
the same test. Each produces a Warning naming the way id and the value.

`footway`, `path`, `cycleway`, `pedestrian`, `steps`, `track`, `bridleway`,
`corridor`, `platform`, `busway`, `bus_guideway`, `escape`, `raceway`,
`construction`, `proposed`, `rest_area`, `services`, `bus_stop`, `crossing`,
`elevator`, `emergency_bay`, `traffic_island`.

Also dropped, whatever the `highway` value:

- ways tagged `area=yes` or carrying `area:highway` — a polygon is a surface, not
  a centreline, and this sprint imports centrelines only;
- ways tagged `access=no` or `access=private`, reported as one aggregated Warning
  with a count rather than one per way.

## 2. Other tags

<!-- rm-osm: tags -->
| Tag | Effect | Dropped |
|---|---|---|
| `name` | road name | no |
| `oneway` | one-way cross section; `-1` reverses the polyline | no |
| `lanes` | driving lanes, split across directions | no |
| `lanes:forward` | driving lanes in the reference-line direction | no |
| `lanes:backward` | driving lanes against the reference-line direction | no |
| `maxspeed` | `<type><speed @max @unit>` | no |
| `junction` | `roundabout`/`circular` imply oneway | no |
| `layer` | topology only — see §4 | no |
| `bridge` | reported, imported at grade | no |
| `tunnel` | reported, imported at grade | no |
| `surface` | — | yes |
| `smoothness` | — | yes |
| `width` | — | yes |
| `sidewalk` | — | yes |
| `turn:lanes` | — | yes |
| `cycleway` | — | yes |
| `lit` | — | yes |
| `ref` | — | yes |

Dropped tag keys are reported **once per import**, aggregated with a count of the
ways carrying each — a per-way diagnostic for `surface` on a 1 600-road district
is 1 600 rows that say the same thing.

### `oneway`

`yes`, `true`, `1` and `-1` are one-way; `no`, `false`, `0` and an absent tag are
two-way. `motorway`, `motorway_link` and roundabouts are one-way by OSM
convention even without the tag — applied, and stated once per import rather than
silently.

**`oneway=-1` reverses the polyline itself**, so the reference line always runs
with traffic. Encoding a reversed lane direction instead would be equally valid
OpenDRIVE and wrong for every downstream editing gesture: `@s` would increase
against travel, and every station-anchored thing a user later places — a stop
line, a signal, a bridge span — would read backwards.

### `lanes`

Parsed as an integer and clamped to a sane range; an unparseable value (OSM
permits `lanes=1;2` on some ways) falls back to the class default with a Warning.
`lanes` on a two-way road splits across directions, remainder to the forward
side. `lanes:forward`/`lanes:backward` override the split when present. **Lane
*counts* come from OSM; lane *widths* always come from the defaults registry.**

### `maxspeed`

Becomes the `<speed>` child of the road's `<type>` record
(OpenDRIVE §10.4.1). The unit follows OSM's own convention, which is that a bare
number is km/h and an imperial value is explicit:

| `maxspeed=` | `@max` | `@unit` |
|---|---|---|
| `50` | `50` | `km/h` |
| `30 mph` | `30` | `mph` |
| `none` | `no limit` | *(omitted)* |
| `walk`, `signals`, `variable` | — dropped with a Warning — | |

`none` is the German autobahn case and maps to the `t_maxSpeed` string literal
`no limit`, which is why that attribute is **not** a plain number in the data
model — see [realism_defaults.md](./realism_defaults.md) §1.7 and the round-trip
test that pins both `no limit` and `undefined`.

## 3. Geometry fitting

An OSM way is a polyline of surveyed or traced points; an OpenDRIVE road is a
sequence of analytic geometry records. The conversion loses information, and each
loss is measured and reported rather than estimated.

The pipeline order is load-bearing:

```
classify tags → reproject nodes (once each) → SPLIT at shared nodes
              → collapse coincident points → simplify → hairpin guard → fit
```

**Splitting happens before simplification**, so a junction node is always a
segment endpoint, and the simplifier always keeps endpoints. A junction can
therefore never be simplified away — a guarantee by construction rather than by a
protected-vertex list that has to be maintained correctly.

| Stage | Rule | Reported as |
|---|---|---|
| collapse | interior points closer than **1.0 m** to their predecessor are merged; endpoints never are | `merged_nodes` |
| simplify | Ramer–Douglas–Peucker at **0.5 m** | `kept_nodes`, `max_deviation_m` |
| cap | over **64 waypoints**, the tolerance doubles and retries, bounded | `tolerance_used_m` |
| hairpin | a turn sharper than ~150° splits the road at that node | `split_at_hairpin` |

**Why 0.5 m.** OSM geometry is traced from imagery at roughly 1–3 m positional
accuracy, so 0.5 m sits *below the source's own noise floor* — the simplification
is not the dominant error term in the result. It is also well under one driving
lane (3.0–3.6 m), so a simplified centreline still lies within its own lane. A
tolerance without a stated derivation is the one that gets quietly re-tuned.

**Why 1.0 m and not a geometric epsilon.** The clothoid fitter refuses coincident
waypoints outright, but a fit through two points 3 mm apart is numerically
degenerate well before it fails. 1.0 m is a road-scale answer to a road-scale
question.

The reported deviation is the **actual** greatest distance from a dropped vertex
to the retained line, not the tolerance that was requested. Quoting the tolerance
would report the question rather than the answer.

## 4. Topology

Roads are joined using OSM's own shared-node graph, never geometric crossing
detection. The graph is exact, the crossing search is an unindexed scan, and at
district scale the difference is the whole import.

| Road ends meeting at a node | Result |
|---|---|
| 1 | free end |
| 2 | a plain link |
| 3 … 8 | a generated junction |
| more than 8 | **refused**, ends left free, Warning naming the node and the count |

The eight-arm cap exists because junction generation produces a connecting road
per permitted (incoming lane, outgoing lane) pair: arms grow the result
quadratically, and a twelve-arm OSM node is a generated-road explosion rather
than an intersection anyone modelled deliberately.

### Arms are pulled back from a junction

**OSM states a crossing by sharing a node**, so every arm of an intersection
runs to exactly the same point. A connecting road between two arms that meet at
a point has zero length and nowhere to curve — so before authoring, each arm of
a **junction** joint is trimmed back **12 m** from the node, leaving the
generator room to build.

This is not a refinement; without it the import produces no junctions at all.
Measured on a 7×7 lattice: 25 junctions planned, **zero built**, every turn
reporting *"clothoid fit failed"* — and the result still looked plausible in the
road count, because the authored roads were all there. With the setback the
same lattice builds 45 junctions and drops no turns.

A **link** joint (degree 2) is *not* trimmed: its two ends must stay coincident
or `check_linkable` refuses and the roads never join at all. A segment too short
to give the metres up is left alone and reported, because a road that vanished
is worse than a joint that reports its dropped turns.

**`layer` partitions the graph before degree is counted.** Two ways sharing a
node at different `layer` values are an overpass and the road beneath it — they
are **not joined**, and a Warning names both ways and both layers. Without this
rule the import silently welds a bridge to the road under it: invisible in plan
view, and wrong in every 3D consumer downstream.

Turns the junction generator could not fit are reported individually — the
generator drops them by design, and a district import that dropped forty turns in
silence would look identical to one that dropped none.

### Roundabouts

`junction=roundabout` is imported as one-way road segments with a junction at
each arm node, **not** as a single roundabout entity. That is a genuine
compromise: it produces correct drivable geometry and loses the fact that the
ring is one thing. Each roundabout emits a Warning naming its segment count and
citing [#495](https://github.com/Robomous/RoadMaker/issues/495).

### Re-importing

A road carries its provenance in its OpenDRIVE id — `osm.<way>.<segment>` — which
also serves as the idempotency key. Importing the same extract twice skips every
road already present and says how many it skipped; it does not produce a second
copy of the district.

## 5. Diagnostics

All import diagnostics use the `robomous.ai:rm:1.0.0:` vendor namespace, because
none of these situations has an ASAM rule: they are facts about a *conversion*,
and OpenDRIVE does not describe where its data came from.

| Rule id | Meaning |
|---|---|
| `osm.fit_approximated` | a polyline was simplified, or nodes were merged |
| `osm.element_dropped` | a way, tag or relation was not imported |
| `osm.topology_unlinked` | a shared node did not become a link or a junction |
| `osm.turn_dropped` | a junction turn the generator could not fit |
| `osm.at_grade` | a bridge or tunnel imported without vertical separation ([#496](https://github.com/Robomous/RoadMaker/issues/496)) |

Individual diagnostics are emitted up to a cap, after which they aggregate by
reason with counts. A 50 km² district can produce tens of thousands of
compromises, and a panel with thirty thousand rows communicates no better than a
panel with none.

**Relations** (`type=restriction`, `route`, `multipolygon`) are counted while
parsing and never used. The count is reported, with turn restrictions called out
separately: a user who took the trouble to map them will look for them, and
should be told directly that they did not survive.

## 6. Attribution

OpenStreetMap data is licensed under the **Open Database License (ODbL) 1.0**,
which requires attribution of any work derived from it. RoadMaker reports the
licence and the source file in the import diagnostics; it does **not** yet write
attribution into the exported `.xodr`, because OpenDRIVE has no free-text header
field for it and an `rm:` userData carrier is a feature under
[ADR-0008](../decisions/0008-persistence-layers-asam-first.md)'s registry rules.
**Attribution remains the user's obligation for now**, and the import says so
rather than leaving it to be discovered. Tracked as
[#497](https://github.com/Robomous/RoadMaker/issues/497).

## References

- [ADR-0012](../decisions/0012-osm-ingest-xml-in-house.md) — format scope, and
  why `.osm.pbf` is refused
- [realism_defaults.md](./realism_defaults.md) — the defaults registry every
  width and every road-class default comes from
- [connection_contract.md](./connection_contract.md) — what a link between two
  road ends guarantees
- [geometry.md](./geometry.md) — the tolerance families
- ASAM OpenDRIVE 1.9.0 §10.4, *Road type*, and §10.4.1, *Speed limits for road
  types*
- OpenStreetMap `highway` key documentation; Open Database License 1.0
