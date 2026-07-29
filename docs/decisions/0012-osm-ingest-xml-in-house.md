# ADR-0012: OSM ingest reads `.osm` XML in-house; `.osm.pbf` is refused by name

*Why OpenStreetMap extraction parses the XML interchange format with the pugixml
this project already links, why the binary `.osm.pbf` container is declined
rather than hand-decoded, and why that decision turns on zlib rather than on
Protocol Buffers.*

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Armando Anaya

## Context

P7's OSM sprint (`p7-s4`,
[#244](https://github.com/Robomous/RoadMaker/issues/244)) is the third import
sprint in a row to open by asking what it is allowed to depend on, and the
second to open holding a ruling it did not make.

[ADR-0010](./0010-gis-ingest-bounded-crs.md) declined PROJ and GDAL, stating the
governing principle: **answer only where the answer can be computed exactly, and
name what is refused rather than guess at it.**
[ADR-0011](./0011-lidar-ingest-in-house-las.md) then declined PDAL, not on its
merits but on its *transitive drag* — PDAL `find_package`s PROJ 9.0, GDAL 3.8 and
libgeotiff as REQUIRED, so taking it would have re-imported, as a rider on a
feature sprint, exactly what ADR-0010 had declined the week before.

`p7-s4` inherits both rulings intact and needs nothing from either. **OSM data is
WGS84 longitude/latitude — `EPSG:4326`, the hub of the bounded family
ADR-0010 already computes in closed form.** There is no new coordinate question
here at all; `gis::crs_transform` handles the whole of it with the same three
calls the GIS and lidar importers make. The only open question is the
*container*.

OpenStreetMap publishes road data in two interchange formats, and they are not
equivalent in what they cost to read.

**`.osm` is XML.** A flat document of `<node>`, `<way>` and `<relation>`
elements with `<tag k= v=>` children. It is what the Overpass API returns by
default, what `osmium cat -o out.osm` writes, and what the JOSM editor saves.
**pugixml 1.16 is already a dependency** — `cmake/deps.cmake` pins it, and
`core/src/xodr/reader.cpp` and `writer.cpp` are built on it. Reading `.osm`
therefore costs **zero new dependencies and zero new build time**.

**`.osm.pbf` is a Protocol Buffers container.** It is what Geofabrik and
Planet.osm distribute, and it is roughly an order of magnitude smaller on disk,
which is why bulk extracts use it. Its structure is a sequence of
`BlobHeader`/`Blob` pairs; each `Blob` carries its payload either as `raw` bytes
or — in every real-world file — as `zlib_data`, and the decompressed payload is a
protobuf-encoded `HeaderBlock` or `PrimitiveBlock` (a string table plus
delta-encoded `DenseNodes`, `Way` and `Relation` messages).

The alternatives considered were: `.osm` XML only; `.osm` XML plus a hand-written
`.pbf` decoder; either of those plus libosmium or the reference `osmpbf` library;
and an editor-side Overpass API client that fetches XML for a bounding box.

## Decision

**OSM ingest reads the `.osm` XML interchange format in RoadMaker's own code,
using the already-linked pugixml, and reprojects through the `roadmaker::gis` CRS
layer unchanged. `.osm.pbf` is refused by name with a diagnostic. There is no new
dependency.**

### Why `.pbf` is declined, and it is not the reason it looks like

The obvious argument — *Protocol Buffers is a heavyweight dependency* — is
**true of the protobuf library and irrelevant to this decision**, because we
would not take that library. The OSM PBF schema uses a handful of field types,
and a decoder for it is varints and length-delimited fields: a few hundred lines,
of exactly the same character as the ASPRS LAS reader ADR-0011 chose to write.
On the protobuf axis alone, `.pbf` passes the test ADR-0011 set.

**It fails on zlib.** Every `.osm.pbf` in circulation stores its blobs as
`zlib_data`, so a `.pbf` reader is a zlib consumer, and **zlib is not a
dependency of this project** — deliberately, and visibly: `cmake/deps.cmake:195`
sets `zlib OFF` for libtiff, and the codecs that need it are the reason Deflate
and JPEG-in-TIFF sit in ADR-0010's refused column rather than failing silently.
Adding zlib is already scoped work: it is
[#484](https://github.com/Robomous/RoadMaker/issues/484), which owns the pinned
tag, the `URL_HASH`, the `THIRD_PARTY_LICENSES.md` row and the build-cost
measurement the [dependency policy](../standards/dependencies.md) requires.

So the shape of the refusal is the one this project has now reached three times:
**the part we actually want drags in work that belongs to another sprint, and
taking it here would be taking it badly.** ADR-0010 said the `proj.db` delivery
work "is that sprint's scope, not a rider on it". The same sentence applies
without modification to zlib and #484.

| | Supported | Refused, by name, with a diagnostic |
|---|---|---|
| **Format** | `.osm` / `.xml` OpenStreetMap XML | `.osm.pbf` ([#494](https://github.com/Robomous/RoadMaker/issues/494) — needs zlib), `.o5m`, `.osc` change files |
| **CRS** | WGS84 geographic (`EPSG:4326`) — OSM's only coordinate system | n/a; OSM has no other |
| **Elements** | `<node>`, `<way>`, and the `highway=*` classification in [osm_mapping.md](../domain/osm_mapping.md) | `<relation>` (counted and reported, never used); every non-road `highway` value; `area=yes` polygons |

The practical cost of the refusal is smaller than the file-size difference
suggests, because **the Overpass API returns `.osm` XML directly** and a
road-network query (`way[highway]` over a bounding box) discards the great
majority of a district's data before it is ever downloaded. A user who starts
from a Geofabrik `.pbf` converts it once with `osmium cat -o district.osm`. The
refusal diagnostic says exactly that, names zlib as the reason, and cites the
issue that would lift it — the same contract ADR-0010's refusals carry.

### What is *not* deferred

The scale half of `p7-s4` is unaffected by any of this. The 1 000-road and
50 km² targets ([#54](https://github.com/Robomous/RoadMaker/issues/54),
superseded) are measured against a district generated in memory, so the harness
never depends on a committed multi-megabyte extract in either format.

## Consequences

**Easier.** `cmake/deps.cmake` does not change at all — the first import sprint
of the three for which that is true. No license review, no `URL_HASH` to pin, no
`THIRD_PARTY_LICENSES.md` row, no measurable build-time cost, and no new CI
configuration. XML is also the format a user can *read*, which matters more than
it looks: when the importer reports that way 28374501 was dropped, the user can
open the file and see why. A `.pbf`-only pipeline makes every diagnostic
unverifiable by the person receiving it.

**Harder.** `.osm` XML is roughly ten times the size of the equivalent `.pbf` and
parses more slowly, so the parse budget in the scale harness is spent on a format
chosen for accessibility over throughput — and the harness says so, rather than
reporting a number that flatters a decision it did not make. Users arriving from
Geofabrik have a conversion step. Both costs are named in the refusal message.

**Follow-ups this creates** (filed with #244). Every refusal diagnostic cites the
issue that would lift it:

- [#494](https://github.com/Robomous/RoadMaker/issues/494) — **`.osm.pbf`
  reading**, unblocked by and sequenced after
  [#484](https://github.com/Robomous/RoadMaker/issues/484)'s zlib. The decoder
  itself is in-house, following the LAS precedent; it slots in at phase 1 of the
  importer only, behind the same `OsmGraph`, so the planner and the command layer
  never learn a second format exists.
- [#495](https://github.com/Robomous/RoadMaker/issues/495) — **roundabouts as
  single junction entities.** `junction=roundabout` ways are imported this sprint
  as one-way segments with a junction per arm node, which is a real fitting
  compromise and is reported as one.
- [#496](https://github.com/Robomous/RoadMaker/issues/496) — **bridges, tunnels
  and `layer` authored vertically.** This sprint imports them at grade and
  *refuses to weld across a layer discontinuity*, which is the correctness half;
  authoring the vertical separation is the feature half.
- [#497](https://github.com/Robomous/RoadMaker/issues/497) — **persisted ODbL
  attribution.** OpenStreetMap data is ODbL-licensed and attribution is a
  condition of use. There is no free-text header field in OpenDRIVE for it and an
  `rm:` userData code is a sub-feature under
  [ADR-0008](./0008-persistence-layers-asam-first.md)'s registry rules, so this
  sprint reports attribution in the import diagnostics and defers persisting it.

One further follow-up belongs to the mesh layer rather than to import:
[#498](https://github.com/Robomous/RoadMaker/issues/498) — the private
`decimate_ring` in `core/src/mesh/surface_boundary.cpp` solves the same
Ramer–Douglas–Peucker problem for closed rings and should be re-expressed on the
public simplifier this sprint adds. It is held out deliberately: changing which
vertices it keeps moves geometry in every surface fill, which is the byte-identity
hazard [#442](https://github.com/Robomous/RoadMaker/issues/442) documented.

**Reversal cost, stated plainly.** Low, and lower than either predecessor's.
`.pbf` support is one new source file behind the existing extension dispatch in
`load_osm`, plus zlib arriving through #484 on its own terms. Nothing in the tag
mapping, the fitting pipeline, the topology pass or the import command is
format-aware, and that is by construction rather than by luck: the phase-1
boundary exists precisely so a second container can be added without any of them
being touched.

**Superseded wording elsewhere.** The roadmap's P7 row and #244's own scope line
name OSM extraction without naming a format, so nothing is contradicted. The
[P7 discovery report](../roadmap/pillars/p7_discovery.md)'s observation that
there is "no GIS, lidar or OSM ingest, and no GDAL/PROJ/PDAL in
`cmake/deps.cmake`" is now discharged in its first clause and — for the third
consecutive sprint — **still true in its second, which remains the point**.

## References

- [#244](https://github.com/Robomous/RoadMaker/issues/244) — p7-s4, OSM
  road-network extraction + scale targets
- [#54](https://github.com/Robomous/RoadMaker/issues/54) — the superseded scale
  targets and measurement harness
- [#484](https://github.com/Robomous/RoadMaker/issues/484) — Deflate/JPEG-in-TIFF
  decoding; the issue that owns zlib's arrival
- [ADR-0010](./0010-gis-ingest-bounded-crs.md) — the bounded CRS family this
  sprint reuses unchanged, and the principle it states
- [ADR-0011](./0011-lidar-ingest-in-house-las.md) — the transitive-drag test this
  record applies a third time
- [ADR-0008](./0008-persistence-layers-asam-first.md) — why import provenance is
  Layer-2 state, and the `rm:` code registry rules
- [Dependency & licensing policy](../standards/dependencies.md)
- [OSM tag mapping](../domain/osm_mapping.md) — the governing table for what a
  `highway=*` value becomes
- OpenStreetMap XML format; OSM PBF format specification (`fileformat.proto`,
  `osmformat.proto`); Open Database License (ODbL) 1.0
