# ADR-0011: Lidar ingest reads LAS in RoadMaker's own code, not PDAL

*Why point-cloud import parses ASPRS LAS itself and takes laz-perf for LAZ,
instead of the PDAL stack the roadmap named — and why that is the same decision
ADR-0010 already made, arriving from a different direction.*

- **Status:** accepted
- **Date:** 2026-07-28
- **Deciders:** Armando Anaya

## Context

P7's lidar sprint (`p7-s3`,
[#243](https://github.com/Robomous/RoadMaker/issues/243)) names its dependency
in its own title. The scope line reads *"Point-cloud ingestion (PDAL) as visual
reference and ground-fitting input"*, and the roadmap's P7 row and the P7
milestone description both say **lidar (PDAL)**. That naming predates
[ADR-0010](./0010-gis-ingest-bounded-crs.md) by nine months.

ADR-0010 was accepted one day before this record. It ruled that GIS ingest
computes a bounded family of projections in closed form and that **PROJ and GDAL
are not dependencies**, on three grounds: a ~9 MB `proj.db` would need runtime
data-delivery mechanisms that exist neither for the three editor bundles nor for
the Python wheels; PROJ's own build wants an `sqlite3` executable, against the
reproducible-from-CMake-alone rule; and nothing in the bounded family the data
actually arrives in needs a database lookup. It also stated, deliberately, that
if PROJ ever does enter, **"the `proj.db` delivery work is that sprint's scope,
not a rider on it"**.

So the first question `p7-s3` has to answer is not *how do we read a point
cloud* but **does PDAL cost what ADR-0010 just declined to pay?**

It does. PDAL's own build files, read at
`github.com/PDAL/PDAL` on 2026-07-28 — not its README:

```cmake
# cmake/proj.cmake
find_package(PROJ 9.0 REQUIRED CONFIG)
# cmake/gdal.cmake
find_package(GDAL CONFIG REQUIRED)          # FATAL_ERROR below GDAL 3.8
# cmake/geotiff.cmake
find_package(GeoTIFF REQUIRED 1.7.0)
```

with zlib, lzma, zstd, libxml2, nlohmann, h3, arbiter, lazperf and utfcpp
alongside them. PROJ is not an optional PDAL feature that can be switched off
the way libtiff's zlib codec was — it is `REQUIRED`, because PDAL delegates all
of its spatial-reference handling to GDAL, which in turn hard-requires PROJ.

Against that sits what a LAS file actually is. ASPRS LAS is a **fixed-width
binary format**: a public header block of 227 bytes (LAS 1.0–1.2), 235 (1.3) or
375 (1.4), a list of variable-length records, and point records of a length the
header states, in one of eleven documented layouts. Its coordinate reference
system travels in a `LASF_Projection` VLR as either GeoTIFF GeoKeys (records
34735/34736/34737) or an OGC WKT string (record 2112) — **both of which
`roadmaker::gis` already parses**, because `p7-s2` had to read the same GeoKeys
out of a GeoTIFF and the same WKT out of a `.prj` sidecar.

The one part of the format that cannot reasonably be written here is LAZ's
entropy coder. LAZ is not a container with a general-purpose compressor inside
it; it is an arithmetic coder with per-field predictive models, and a
from-scratch implementation would be a sprint of its own with no way to be sure
it was right.

The alternatives considered were: PDAL as named; an in-house LAS reader with
`.laz` refused by name; an in-house LAS reader plus laz-perf for `.laz`; and
PDAL behind an OFF-by-default option in the `RM_BUILD_USD` pattern.

## Decision

**Lidar ingest reads ASPRS LAS 1.0–1.4 in RoadMaker's own code, reusing the
`roadmaker::gis` CRS layer unchanged. PDAL, GDAL and PROJ are not dependencies.
The only new dependency is laz-perf, used solely as the LAZ chunk
decompressor.**

This is ADR-0010's governing principle applied a second time — *answer only
where the answer can be computed exactly, and name what is refused rather than
guess at it* — and its eighth evaluation criterion firing exactly as written:
**whether the part you actually want drags the rejected dependency back in.**
What `p7-s3` wants from PDAL is point records out of a binary file. What it
would have to accept to get them is the entire PROJ delivery problem ADR-0010
deferred to [#485](https://github.com/Robomous/RoadMaker/issues/485), taken as a
rider on a feature sprint — which is the thing ADR-0010 named as *"how it would
get done badly"*.

| | Supported | Refused, by name, with a diagnostic |
|---|---|---|
| **Container** | `.las` (LAS 1.0, 1.1, 1.2, 1.3, 1.4); `.laz` (the same, LAZ-compressed) | LAS 1.5 and later (#490); `.e57`, `.ply`, `.pts`, `.xyz` |
| **Point records** | formats 0–10 | waveform packet records and EVLRs — skipped, and counted in a warning |
| **CRS** | whatever `gis::parse_crs` accepts: WGS84 geographic, UTM, Transverse Mercator, Web Mercator | every other CRS — carried verbatim as opaque, import declined (#485) |
| **Derivation** | a decimated display cloud; a ground-fitted `HeightField` | automatic feature extraction — out of scope by #243 itself |

Reading LAS ourselves is not merely cheaper here, it is *better positioned*: the
refusal wording, the diagnostics and the CRS family stay identical across all
three P7 importers because they come from the same `gis::crs_transform` call,
rather than from a second library with its own opinion about what it can
project.

**laz-perf is a fundamentally different proposition from PDAL**, in the way
libtiff was from PROJ. [hobuinc/laz-perf](https://github.com/hobuinc/laz-perf)
3.4.0 is **Apache-2.0** — literally on the allowed-license list, so unlike
libtiff it needs no per-case approval and carries no active attribution
obligation beyond the licence's own notice terms. Its root `CMakeLists.txt`
calls `find_package` **zero times**: it has no external dependencies at all. It
is C++17, builds in seconds, and ships no runtime data files. Two of its
upstream defaults must be forced off and the reason recorded at the pin:
`BUILD_SHARED_LIBS` defaults `TRUE`, and `WITH_TESTS` defaults `TRUE` with a
`file(DOWNLOAD ...)` of a sample tile **at configure time** — a network fetch in
a build that must be reproducible offline.

laz-perf is used as the chunk **decompressor** only. The public header, the
VLRs and the point-record layouts are parsed by RoadMaker, so `.las` and `.laz`
converge on one code path immediately after decompression and the diagnostics
are ours in both cases. That also means a laz-perf that ever became unavailable
would cost `.laz` support and nothing else.

## Consequences

**Easier.** The kernel stays exactly as ADR-0010 left it: pure Apache-2.0,
buildable from CMake alone, shipping zero runtime data, with no OFF-by-default
option to gate and no second CI job. A LAS reader is a few hundred lines of
offset arithmetic against a published table, testable against committed fixtures
this repository generates itself — the same shape of asset the GIS fixtures
already are, for the same reason (provenance is unambiguously ours and no GIS
stack is needed to regenerate them).

**Harder.** Eleven point-record layouts and four header sizes have to be got
right by reading the specification rather than by delegating, and a mistake in
an offset table is the kind of bug that reads plausible data from the wrong
bytes. That is answered by fixtures at each version and format, and by decoding
the `.laz` and `.las` forms of the *same* points and asserting they agree — the
oracle `p7-s2` used to validate its own hand-written LZW encoder.

A second consequence worth stating: a point cloud that is refused for its CRS is
refused after its header has been read, not before, so the diagnostic can name
the projection the file declared. That is the whole point of the bounded-family
design and it costs one header read.

**Follow-ups this creates.** Every refusal diagnostic cites the issue that would
lift it:

- [#490](https://github.com/Robomous/RoadMaker/issues/490) — `p7-f4`: LAS 1.5
  and later, whose CRS representation is WKT-only and whose header grows again.
- [#485](https://github.com/Robomous/RoadMaker/issues/485) — arbitrary-CRS
  reprojection, unchanged and still **to be opened only when a real consumer
  needs it**. `p7-s3` is not that consumer: it reprojects through the same
  bounded family `p7-s2` built.

**Reversal cost, stated plainly.** Nothing here forecloses PDAL. If a consumer
appears that genuinely needs PDAL's filter pipeline — ground classification,
outlier removal, tiling — PDAL enters behind an OFF-by-default `RM_BUILD_PDAL`
following the `RM_BUILD_USD` pattern, and it arrives *after* #485 has solved
`proj.db` delivery, not instead of it. The in-house reader stays as the
no-dependency default, exactly as the bounded CRS path does. Reading LAS
ourselves costs nothing if that day comes; taking PDAL now would cost the
packaging work immediately, and buy filters nothing in `p7-s3` asks for.

**Superseded wording elsewhere.** The roadmap's P7 row, the P7 milestone
description, the [2026-07 realignment](../roadmap/updates/2026-07-realignment.md)
and [ADR-0006](./0006-terrain-scope.md)'s import-stack references all name PDAL
as the vehicle, as does `p7-s3`'s own issue title. They are amended by this ADR;
the *outcome* they describe — a lidar tile as visual reference and as
ground-fitting input — lands unchanged, by a different route.
[p7_discovery](../roadmap/pillars/p7_discovery.md)'s observation that there is
"no GDAL/PROJ/PDAL in `cmake/deps.cmake`" remains true after this sprint, which
is the point.

## References

- [#243](https://github.com/Robomous/RoadMaker/issues/243) — p7-s3, lidar import
- [ADR-0010](./0010-gis-ingest-bounded-crs.md) — GIS ingest on a bounded CRS
  family; the record this one applies a second time
- [ADR-0008](./0008-persistence-layers-asam-first.md) — why an imported point
  cloud is Layer-2 reference state, while the height field it seeds is not
- [ADR-0006](./0006-terrain-scope.md) — terrain scope; the height field a
  ground fit writes into
- [Dependency & licensing policy](../standards/dependencies.md)
- ASPRS *LAS Specification* 1.4 R15 — public header block, variable-length
  records, point data record formats 0–10, and the `LASF_Projection` CRS records
- [hobuinc/laz-perf](https://github.com/hobuinc/laz-perf) — Apache-2.0 LAZ codec
