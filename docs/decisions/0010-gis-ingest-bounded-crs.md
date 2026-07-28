# ADR-0010: GIS ingest on a bounded CRS family, not PROJ

*Why GIS import computes a named set of projections in closed form and refuses
everything else by name, instead of taking PROJ (or GDAL) as a dependency — and
exactly what it would take to change course.*

- **Status:** accepted
- **Date:** 2026-07-28
- **Deciders:** Armando Anaya

## Context

P7's GIS sprint (`p7-s2`,
[#242](https://github.com/Robomous/RoadMaker/issues/242)) does not begin from a
blank slate. It begins holding a decision that `p7-s5`
([#324](https://github.com/Robomous/RoadMaker/issues/324)) deliberately left
open, in the kernel's own words
(`core/include/roadmaker/road/georeference.hpp`):

> §8.5 describes a geodetic datum with a PROJ string, and converting BETWEEN
> two such datums needs the PROJ library. PROJ is not a dependency of this
> project and is deliberately deferred to p7-s2 (#242, GIS import), where the
> first consumer that genuinely has to transform coordinates between two
> different reference systems appears. Bringing it in here would add SQLite plus
> a runtime `proj.db` resource to a kernel that builds from CMake alone and
> ships no runtime data, to serve no caller.

That deferral was written to be revisited exactly here, so the first question
#242 has to answer is not *how do we import a shapefile* but **does the
consumer that just appeared actually need PROJ?**

Three constraints frame the answer. First, the
[dependency policy](../standards/dependencies.md) says to question anything
adding more than ~30 seconds to a clean build, to stop and ask before adding a
heavyweight, and that builds must be reproducible from CMake alone — no system
packages for libraries the kernel links. Second, **nothing in this repository
ships a loose runtime data file**: `core/` ships none by design, the Python
wheel installs only the compiled extension module, and the one multi-megabyte
runtime file that exists at all (the Qt Help collection) is editor-only and
staged by Qt's own deploy tooling. A ~9 MB `proj.db` would need a new delivery
mechanism for macOS, Windows and Linux bundles *and* a second, entirely new one
for wheels. Third, PROJ's own build wants an `sqlite3` executable to construct
that database, which pushes directly against the reproducible-from-CMake-alone
rule.

Against those costs sits what the data actually looks like. Road-authoring GIS
data arrives in WGS84 geographic coordinates or in a Transverse Mercator zone —
overwhelmingly UTM — and `p7-s5` already established that RoadMaker's own world
frame *is* a Transverse Mercator on the scene origin, per §8.5's own
recommendation. Transforming between members of that family is closed-form
arithmetic, not a database lookup.

The alternatives considered were: bounded family with no new dependency at all;
bounded family plus libtiff for real GeoTIFF; PROJ with SQLite3 and `proj.db`;
and full GDAL behind an OFF-by-default option in the `RM_BUILD_USD` pattern.

## Decision

**GIS ingest supports a named, bounded family of coordinate reference systems,
computed in closed form, and refuses everything outside it by name with a
diagnostic. PROJ and GDAL are not dependencies. The only new dependency is
libtiff.**

The governing principle is the one `tmerc_origin()` already follows, widened to
a family: **answer only where the answer can be computed exactly, and name what
is refused rather than guess at it.** `tmerc_origin()` returns `nullopt` for a
UTM string precisely because guessing would be wrong by that projection's
500 km false easting; a GIS importer that quietly mis-georeferenced an
orthophoto would be the same defect with a picture attached.

| | Supported | Refused, by name, with a diagnostic |
|---|---|---|
| **CRS** | WGS84 geographic; UTM north/south zones; Transverse Mercator; Web Mercator | every other CRS — carried verbatim as opaque, import declined |
| **Vector** | GeoJSON (RFC 7946); ESRI Shapefile (`.shp`/`.shx`/`.dbf`/`.prj`) | GeoPackage, KML, DXF |
| **Raster** | GeoTIFF (uncompressed, PackBits, LZW); PNG/JPEG with a world file | Deflate and JPEG-in-TIFF (need zlib/libjpeg); JP2, ECW, MrSID |

WGS84 and GRS80 are treated as one ellipsoid: they differ by about 0.1 mm in
the semi-minor axis, which is four orders of magnitude below anything this
editor authors. No datum *shift* is performed or claimed — a source in a
national datum that genuinely needs a grid-shift transform is refused, not
approximated.

Raster support needs a TIFF decoder, and **libtiff is a fundamentally different
proposition from PROJ**: about a megabyte of source, no runtime data files, no
transitive dependencies once its optional codecs are switched off, and a build
measured in seconds. Its codecs that require zlib or libjpeg stay off, which is
why Deflate and JPEG-in-TIFF appear in the refused column rather than silently
failing. GeoTIFF's GeoKeys are parsed in RoadMaker's own code rather than via
libgeotiff, because libgeotiff's EPSG resolution is exactly the part that wants
PROJ back.

**License due diligence** (`LICENSE.md` in the upstream repository, read
2026-07-28, not the README badge). libtiff is BSD-style: copyright Sam Leffler
and Silicon Graphics, permission to "use, copy, modify, distribute, and sell
this software and its documentation for any purpose" without fee, provided the
copyright notices survive and the holders' names are not used in advertising
without permission. Its SPDX identifier is `libtiff`, which is **not literally
on the allowed-license list**; in substance it is at least as permissive as the
BSD-2 entry that is. **Maintainer-approved 2026-07-28**, recorded in
`THIRD_PARTY_LICENSES.md`, on the same footing as tinyusdz's vendored
ISC/Unlicense components under [ADR-0005](./0005-tinyusdz-usda.md).

One clause needs naming rather than summarising: **the LZW code (`tif_lzw.c`)
carries an additional Regents of the University of California copyright whose
terms require that documentation and other materials distributed with the
software acknowledge that the software was developed by UC Berkeley.** That is
an active obligation, not boilerplate, and it is discharged by an explicit
acknowledgement line in `THIRD_PARTY_LICENSES.md` — which must survive any
future edit of that file.

## Consequences

**Easier.** The kernel stays exactly as it was after `p7-s5`: pure Apache-2.0,
buildable from CMake alone, shipping zero runtime data. Wheels need no new
packaging mechanism. There is no per-driver GPL audit to run, no OFF-by-default
build option to gate, no second CI job, and no platform-specific data staging.
The CRS code is a few hundred lines of testable arithmetic with committed
fixtures rather than an opaque database whose answers we could not check.

**Harder.** Anything outside the supported family stops at the door. A user
holding a national-grid dataset must reproject it themselves before importing —
so the refusal diagnostic has to be genuinely useful, naming the CRS it read and
pointing at the follow-up issue, not merely reporting failure.

**Follow-ups this creates** (filed with #242, all `pillar:P7`). Every refusal
diagnostic cites the issue that would lift it, so a user who hits one is told
where the limitation is tracked rather than merely that it exists:

- [#484](https://github.com/Robomous/RoadMaker/issues/484) — Deflate and
  JPEG-in-TIFF decoding, which needs zlib and libjpeg.
- [#485](https://github.com/Robomous/RoadMaker/issues/485) — arbitrary-CRS
  reprojection, the PROJ escape hatch described below. **To be opened only when
  a real consumer needs it**, which is the whole point of this record.
- [#486](https://github.com/Robomous/RoadMaker/issues/486) — additional vector
  formats (GeoPackage, KML).

**Reversal cost, stated plainly.** Nothing here forecloses PROJ. If a consumer
appears that genuinely needs arbitrary datum transforms, PROJ enters behind an
OFF-by-default `RM_BUILD_PROJ` following the `RM_BUILD_USD` pattern, and
`gis::crs_transform` gains a second implementation behind the same signature —
the bounded path stays as the no-dependency default. **The `proj.db` delivery
work is that sprint's scope, not a rider on it**: editor bundles on three
platforms, a wheel data-file mechanism that does not exist today, and a runtime
search path the kernel currently has no concept of. Bundling that work into a
feature sprint is how it would get done badly, which is the same judgement
`p7-s5` made when it declined to do it there.

**Superseded wording elsewhere.** The roadmap's P7 row, the
[2026-07 realignment](../roadmap/updates/2026-07-realignment.md), GW-2's step 27
("PROJ arrives with p7-s2") and [ADR-0006](./0006-terrain-scope.md)'s
import-stack rationale all name GDAL/PROJ as the vehicle. They are amended by
this ADR; the *outcomes* they describe — georeferenced import, DEM ingest,
reprojection into the world frame — all still land, by a different route.
[p5_discovery](../roadmap/pillars/p5_discovery.md)'s decision D1 promised
"GDAL rasters (GeoTIFF etc.) joining in P7"; GeoTIFF does join in P7, via
libtiff, so D1 is discharged rather than broken.

## References

- [#242](https://github.com/Robomous/RoadMaker/issues/242) — p7-s2, GIS
  vector/raster import
- [#324](https://github.com/Robomous/RoadMaker/issues/324) — p7-s5, the world
  georeference and the deferral this record answers
- [Dependency & licensing policy](../standards/dependencies.md)
- [ADR-0005](./0005-tinyusdz-usda.md) — the precedent for approving a
  near-list license, and for an ingest/export backend chosen on build cost
- [ADR-0006](./0006-terrain-scope.md) — terrain scope; its DEM ingest path
- [ADR-0008](./0008-persistence-layers-asam-first.md) — why imported reference
  layers are Layer-2 state and never enter the `.xodr`
- ASAM OpenDRIVE 1.9.0 §8.5, *Geo-referencing*
- RFC 7946 (GeoJSON); ESRI Shapefile technical description; OGC GeoTIFF 1.1
