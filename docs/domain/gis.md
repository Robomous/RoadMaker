# GIS import

*What RoadMaker reads from GIS data, which coordinate reference systems it
computes with, and — just as importantly — what it declines and why.*

The reasoning behind every boundary on this page is
[ADR-0010](../decisions/0010-gis-ingest-bounded-crs.md). The short version:
RoadMaker computes a **bounded family** of projections in closed form and
**refuses everything else by name**. There is no GDAL and no PROJ.

## Coordinate reference systems

| Supported | Spellings recognised |
|---|---|
| WGS 84 geographic (lon/lat degrees) | `EPSG:4326`, `+proj=longlat`, a WKT `GEOGCS` on WGS 1984 |
| UTM, north and south zones | `EPSG:326xx` / `EPSG:327xx`, `+proj=utm +zone=…`, ESRI WKT |
| Transverse Mercator | `+proj=tmerc …`, WKT `PROJECTION["Transverse_Mercator"]` |
| Web Mercator | `EPSG:3857` (also `900913`, `102100`), `+proj=merc` on the WGS 84 sphere |

Transverse Mercator is evaluated with the **Krüger series** to sixth order in
the third flattening, which holds sub-millimetre accuracy across a zone's range
of validity. UTM is a Transverse Mercator with fixed parameters, so both go
through the same arithmetic. Everything routes through geographic lon/lat as a
hub, so each projection has exactly one forward and one inverse.

**WGS 84 and GRS 80 are treated as the same ellipsoid.** They differ by about
0.1 mm in the semi-minor axis, four orders of magnitude below anything this
editor authors. This is an *ellipsoid* equivalence, not a *datum* one — a CRS on
a different datum (NAD 83, OSGB 36, a national grid) is **not** supported,
because reconciling those needs a shift this build does not perform.

**No datum shift is ever performed or claimed.** Anything outside the table is
carried verbatim, reported as opaque, and refused at the point a transform would
be needed — naming the CRS it read and citing
[#485](https://github.com/Robomous/RoadMaker/issues/485), the issue that would
lift the limitation.

Linear units must be metres. A CRS in feet is refused, not converted: silently
reading US survey feet as metres is precisely the class of error this design
exists to prevent.

## Formats

| Kind | Read | Refused |
|---|---|---|
| Vector | GeoJSON (`.geojson`, `.json`), ESRI Shapefile (`.shp`) | GeoPackage, KML, DXF ([#486](https://github.com/Robomous/RoadMaker/issues/486)) |
| Raster | GeoTIFF (`.tif`, `.tiff`) — uncompressed, PackBits, LZW | Deflate, JPEG-in-TIFF ([#484](https://github.com/Robomous/RoadMaker/issues/484)), JP2, ECW, MrSID |
| Raster | PNG/JPEG with a world file (`.pgw`/`.jgw`/`.wld`) and `.prj` | an image with no georeferencing at all |

- **GeoJSON** is WGS 84 longitude/latitude by RFC 7946, so it has no CRS to
  state and none to get wrong. A file carrying the pre-RFC `crs` member is read
  as WGS 84 anyway, with a warning.
- **Shapefile** takes its CRS from the sibling `.prj`, its labels from the
  `.dbf`, and needs neither. A missing `.prj` is a warning, and the coordinates
  are then treated as already being in the scene's frame.
- **GeoTIFF** positioning comes from `ModelPixelScale` + `ModelTiepoint`, or
  from `ModelTransformation`. Its CRS comes from the GeoKey directory, parsed
  in RoadMaker's own code — `libgeotiff` is skipped because its EPSG resolution
  is exactly the part that wants PROJ back.
- A **single-band** raster that is 16/32-bit or floating point is read as
  **elevation**; anything else is imagery. An 8-bit single band is a greyscale
  picture, not a DEM quantised to 256 heights.

Readers never drop input silently. A skipped feature, an unreadable record, a
coerced value, or an ignored tag each produce a `Diagnostic`, exactly as the
OpenDRIVE parser does.

## Placed or resampled — a distinction you can see

When the source and scene projections differ only by scale and false origin —
two Transverse Mercators on the **same central meridian** — the mapping is
affine, and a raster is **placed**: its pixels are untouched.

When the mapping curves (different central meridians, or geographic into
projected), the raster is **resampled** bilinearly onto an axis-aligned grid of
**square** pixels, and that is reported as a diagnostic. Silence there would let
a resampled image pass for the source file.

The square-pixel guarantee is load-bearing rather than cosmetic: the scene
height field is a uniform square grid, so an elevation raster resampled to
merely *near*-square pixels would be refused one call later.

## Where imported data lives

Imported vectors and imagery are **authoring reference** — a backdrop to trace
over. Per [ADR-0008](../decisions/0008-persistence-layers-asam-first.md) that is
Layer-2 state: it lives in the scene container and **never enters the `.xodr`**.
Adding or removing a reference layer is therefore not an undoable command.

The one exception is **elevation**, which becomes the scene's `HeightField`
through the same `edit::set_terrain_field` command a `.asc` DEM import uses —
so it is real scene content, undoable, and persisted like any other terrain.

## From Python

```python
import roadmaker as rm

network = rm.RoadNetwork()
geo = rm.GeoReference()
geo.projection = rm.tmerc_projection(52.3702, 4.8952)
network.set_georeference(geo)

layer, diagnostics = rm.gis.load_vector("roads.shp")
transform = rm.gis.crs_transform(rm.gis.parse_crs(layer.crs),
                                 rm.gis.scene_crs(network.georeference))
layer = rm.gis.reproject_vector(layer, transform)
```

A full worked example, including the elevation and refusal paths, is
[`python/examples/gis_import.py`](../../python/examples/gis_import.py).

## Point clouds

Lidar tiles are the third thing that imports through this CRS layer
(`roadmaker::lidar`, `lidar/point_cloud.hpp`). ASPRS **LAS 1.0–1.4** and **LAZ**
are read by RoadMaker itself rather than through PDAL, which is BSD-3 but
hard-requires PROJ 9.0 and GDAL 3.8 — the dependency ADR-0010 declined. The
decision, and what would reverse it, is
[ADR-0011](../decisions/0011-lidar-ingest-in-house-las.md). `laz-perf`
(Apache-2.0) decodes the compressed stream and does nothing else.

| | Supported | Refused, by name, with a diagnostic |
|---|---|---|
| Container | `.las` and `.laz`, LAS 1.0 through 1.4 | LAS 1.5 and later (#490); `.e57`, `.ply`, `.pts` |
| Point records | formats 0–10, at whatever record length the header declares | waveform packet references and EVLRs — skipped, and counted in a warning |
| CRS | whatever `parse_crs` reads out of a `LASF_Projection` record: the GeoTIFF GeoKey directory (34735) or OGC WKT (2112) | every CRS outside the family above (#485) |

Three rules are worth knowing before reading the code:

- **Decimation is decided from the header, before any point is read.** A tile too
  large to hold is also too large to read and then thin, so `stride` comes from
  the declared point count and is deterministic — every *n*th point, never a
  random sample, so two reads of one file agree. The ratio is always reported: a
  cloud silently reduced to a twelfth of itself looks like a sparse survey.
- **Coordinates are floats relative to a double origin.** At a UTM northing near
  5.8 × 10⁶ m a float's quantum is about half a metre, so a cloud stored in
  absolute floats would terrace itself into visible steps. `PointCloud::point()`
  hands back absolute doubles.
- **A ground fit is a binning fit, not a classifier.** When the file classifies
  bare ground (ASPRS class 2) the fit takes the **mean** of those returns per
  post; when it does not, it falls back to the **lowest** return, which rejects
  roofs and canopy at the cost of reading half a cell of grade low on a slope.
  Which one ran is always named in a diagnostic, because the two disagree under a
  bridge. Posts no return landed in are **interpolated from their neighbours,
  never written as zero** — zero is a claim that the ground is at the vertical
  datum, not a missing value.

A tile becomes terrain through the ordinary `edit::set_terrain_field` command, so
it is real, undoable, persisted scene content — the same door a `.asc` DEM and an
elevation GeoTIFF go through. The cloud itself stays Layer-2 reference geometry.

In the editor it is the third `ReferenceLayerKind`, imported with **File ▸
Import ▸ Point Cloud…** and turned into terrain with **Edit ▸ Terrain ▸ Seed
from Point Cloud…**. Two things differ from an imagery underlay:

- **It is not flattened.** Vectors and rasters are backdrops and draw at a fixed
  height just above the ground. A cloud has real Z and draws depth-sorted with
  the scene, because its heights are the reason it was imported.
- **Its vertices stay in the cloud's own frame,** with `PointCloud::origin`
  travelling as the draw's single instance matrix. Baking the origin into the
  vertex buffer would put a scene-scale value back into a float and reintroduce
  the terracing the offset representation exists to prevent — the same defect,
  one layer further out.

The persisted `kind` in the scene container was a two-state `"vector"`/`"raster"`
before this; sidecars written by older builds load unchanged, and an unknown
spelling still reads as a raster with a warning rather than dropping the layer.

## Test fixtures

The committed fixtures in `core/tests/data/gis/` are generated by
[`scripts/gen_gis_fixtures.py`](../../scripts/gen_gis_fixtures.py) using the
Python standard library alone, and those in `core/tests/data/lidar/` by
[`scripts/gen_lidar_fixtures.py`](../../scripts/gen_lidar_fixtures.py) the same
way. They are committed, rather than built inside the tests, because a format
reader checked only against bytes the same test just constructed agrees with
itself about everything — including its mistakes.

One of them, `precision_probe.las`, exists only to make a claim falsifiable. The
main tile sits on a 5 m lattice, and at its own northing a float's quantum is
exactly 0.5 m — so every one of its coordinates is exactly representable even in
an absolute float, and a reader that threw the double origin away would still
round-trip it perfectly. The probe carries centimetre offsets instead, which no
float at that magnitude can hold.

The one exception is `amsterdam_tile.laz`, which needs the codec: it is written
once by `core/tests/tools/make_laz_fixture.cpp` and committed, and
`LazDecodesIdenticallyToLas` compares it against the uncompressed tile point for
point — and CRS for CRS, because laz-perf's writer emits only its own records and
the first version of that tool silently produced a `.laz` with no coordinate
system at all.

## See also

- [ADR-0010](../decisions/0010-gis-ingest-bounded-crs.md) — the decision and its
  reversal cost
- [ADR-0011](../decisions/0011-lidar-ingest-in-house-las.md) — the same decision
  reached again for lidar, and why PDAL is not the vehicle
- [Geo-referencing a scene](./opendrive.md) — OpenDRIVE §8.5 and the world frame
  imports land in
- [Dependencies and licensing](../standards/dependencies.md) — libtiff's
  near-list approval and its attribution obligation
