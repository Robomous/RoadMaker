# ADR-0013: user assets arrive as glTF and images, and a textured model imports flat

*Why the first import sprint that takes no new dependency at all still needs a
record; why a user's own PNG is decoded twice by two different libraries on
purpose; and why an imported model's texture is thrown away with a warning
instead of being carried into a representation that has nowhere to put it.*

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Armando Anaya

## Context

P6's import sprint (`p6-s8`,
[#322](https://github.com/Robomous/RoadMaker/issues/322)) is the fourth import
sprint in a row, and the first whose format question was already settled before
it opened. Realignment **Q7** (resolved 2026-07-20) ruled: glTF/GLB read via the
in-tree tinygltf, images via Qt, **OBJ out** (tinyobjloader would be a new pinned
dependency, and "not cheap enough to smuggle in"), **USD read out** for v0.1.0
("validating arbitrary USD input is a project of its own"), FBX permanently
excluded by the [licensing rules](../standards/dependencies.md).

So unlike [ADR-0010](./0010-gis-ingest-bounded-crs.md) (declined PROJ and GDAL),
[ADR-0011](./0011-lidar-ingest-in-house-las.md) (declined PDAL on its transitive
drag) and [ADR-0012](./0012-osm-ingest-xml-in-house.md) (declined `.osm.pbf` on
zlib), this sprint has **no dependency argument to make**. `cmake/deps.cmake` does
not change, for the second time running. tinygltf 3.0.0 is already pinned and
already linked `PRIVATE` on `roadmaker_core` (`cmake/deps.cmake:89-95, 412-422`;
`core/CMakeLists.txt:151`), its read side already compiles, and `stb_image`'s
implementation already lives in core (`core/src/gis/world_file.cpp:36-43`).

What this sprint has instead is a **representation** problem, and it is the reason
this record exists. Everything else here — the closed format list, the untrusted
input budgets, where imported bytes live on disk — is policy that a reader of the
importer will otherwise have to reconstruct from the code.

### The representation problem, stated exactly

A prop model in RoadMaker is a list of flat-shaded parts:

```cpp
// core/include/roadmaker/assets/prop_library.hpp:40-46
struct PropPart {
  std::vector<double> positions;
  std::vector<double> normals;
  std::vector<std::uint32_t> indices;
  std::array<float, 3> color;   // flat, linear RGB
  std::string name;
};
```

**No UVs. No texture.** And that is not a local gap that can be closed in the
importer, because every consumer of a prop model agrees with it:

| Consumer | Assumption |
|---|---|
| `editor/src/viewport/viewport_widget.cpp:625` | **all** props draw with one shared `material_for(SurfaceKind::Untextured, {})` |
| `editor/src/render/scene_builder.cpp:245-249` | a batch converts parts to `RenderMeshData` once, colour only |
| `core/src/io/gltf_exporter.cpp` | one glTF material per `(model_id, part.name)`, flat colour (`mesh_export_common.hpp:130`) |
| `core/src/io/usd_exporter.cpp:63-71` | its own local `MaterialDef{color3, roughness}` |

A real-world GLB, meanwhile, usually puts *all* of its colour in a
`baseColorTexture` and leaves `baseColorFactor` at white. So the naive import —
read `baseColorFactor`, ignore the image — produces a **white chair**, which is
not a compromise but a bug wearing a compromise's clothes.

The alternatives considered were: factor-only (the white chair); carry UVs and
images through `PropPart`, the batch upload, the viewport material and both
exporters; and decode the image at import time to derive one representative
colour per part.

## Decision

**`p6-s8` reads glTF and GLB into the existing flat-shaded prop representation. A
primitive's `baseColorTexture` is decoded, averaged, multiplied by
`baseColorFactor`, and stored as that part's single flat colour, with a warning
naming the primitive, the material and the resulting colour. `PropPart` is
unchanged, and neither exporter nor the instanced render path is touched. No new
dependency is taken.**

### Why flattening, and why it is not the same as giving up

Averaging a texture is a lossy answer to "what colour is this?", but it is the
*right kind* of lossy: it is computed from the user's actual data, it is
deterministic, and it is reported. The user sees a brown chair where they supplied
a brown chair. Contrast the two rejected options:

- **Factor-only** is cheaper and strictly worse: it produces a white chair from a
  brown one and cannot tell the user why, because from the importer's point of
  view nothing went wrong.
- **Full texture support** is the correct answer to a different question. It
  changes the kernel representation, the instanced upload path, the viewport's
  per-batch material, and **both** exporters — the last of which crosses the
  export byte-identity surface that every bundled prop is currently pinned
  against. That is a sprint, not a rider on an import sprint, and it is now
  [#507](https://github.com/Robomous/RoadMaker/issues/507). **The flatten warning
  cites that issue by number**, so the user is told what they lost, that it is
  known, and where it is tracked.

This follows the principle ADR-0010 stated and the two records after it applied:
*answer only where the answer can be computed exactly, and name what is refused
rather than guess at it.* Here the exact-answer half is the geometry — positions,
normals, indices and the frame conversion are all exact — and the named-refusal
half is the texture.

### The closed format list

| | Supported | Refused, by name, with a diagnostic |
|---|---|---|
| **Models** | `.glb` (binary glTF 2.0), `.gltf` (JSON glTF 2.0) | `.obj` ([#511](https://github.com/Robomous/RoadMaker/issues/511) — needs a new pinned dependency), `.usd`/`.usda`/`.usdc` (Q7: validating arbitrary USD is its own project), **`.fbx` — permanently, proprietary SDK, forbidden by the dependency policy** |
| **Images** | PNG, JPEG (guaranteed); whatever else the decoder on that side of the layer boundary happens to handle | nothing by name; an undecodable image is reported as undecodable |
| **Model contents** | `TRIANGLES` primitives, node hierarchies, `baseColorFactor`, `baseColorTexture` (flattened, above) | non-triangle primitive modes, skins, animations, morph targets, cameras, lights, `KHR_*` extensions — each **counted and reported**, never silently dropped |

### A user's PNG is decoded by two different libraries, deliberately

This looks like an inconsistency and is a consequence of the architecture rule
that `core/` never links Qt:

- **Inside a GLB**, the image is kernel-side data, so it is decoded by
  `stb_image` — whose implementation already exists in core for the GeoTIFF
  reader. tinygltf is built with `TINYGLTF_NO_STB_IMAGE`
  (`cmake/deps.cmake:419`), so the importer installs its own `SetImageLoader`
  rather than getting one for free. **It must not define
  `STB_IMAGE_IMPLEMENTATION` a second time**; the existing one in
  `core/src/gis/world_file.cpp` is the only one, and a second is an ODR
  violation.
- **A standalone texture file** is editor-side data on its way to becoming a
  material the editor renders, so it is decoded by Qt, exactly as Q7 ruled, and
  scaled at decode time via `QImageReader::setScaledSize` — the trick
  `editor/src/document/project_files_model.cpp:60-77` already uses so a 4K
  texture never costs a full-resolution `QImage` for a 32-px row.

Neither decoder crosses the boundary, and no image is decoded twice in one
operation.

### Untrusted input

Both importers read files the user obtained from somewhere else, so the same
posture as the three preceding import ADRs applies, with one addition specific to
glTF.

**Stated budgets, refused with the limit named.** `kMaxPropTriangles`,
`kMaxPropVertices`, `kMaxPropParts`, `kMaxPropImageTexels` — in the idiom of
`gis::kMaxRasterTexels` and `lidar::kMaxCloudPoints`, whose comment states the
reasoning this project has settled on: *refusing that with a stated limit is
honest, where attempting it is an out-of-memory crash blamed on the user's file.*

**External image URIs are directory-scoped.** A `.gltf` may reference its images
by relative path. tinygltf is built with `TINYGLTF_NO_EXTERNAL_IMAGE`, so it will
not read them — **we do**, which makes the path check ours to make and not
something inherited from the library. An image `uri` is resolved only within the
`.gltf`'s own directory: an absolute path, a URL scheme, or any `..` that escapes
that directory is refused with a diagnostic naming the offending `uri`. A
malicious `.gltf` must not be able to make RoadMaker read
`../../../../etc/passwd` and hand its bytes to an image decoder.

**Everything else is a diagnostic, not a crash.** Truncated containers, lying
chunk lengths, out-of-range accessors, cyclic node graphs and non-finite
coordinates all produce an `Expected` error or a `Diagnostic`, and are covered by
fuzz-adjacent tests as #322's acceptance requires.

### Imported bytes are copied into the project

An import **copies** the source file into `<project>/assets/textures/` or
`<project>/assets/props/`, and the overlay manifest entry records the original
absolute path as `source` alongside the user's `license` attestation.

The alternative — referencing the file where the user keeps it — fails the
acceptance criterion that assets survive project close and reopen, because it
fails the first time the user tidies their Downloads folder. Copying makes a
project self-contained, which is also what makes it shareable. The recorded
`source` is provenance, not a dependency: nothing reads it to load the asset.

Two consequences worth naming. Import **refuses to overwrite an existing asset
slug**, offering a suffixed name instead — which keeps every imported file at a
unique path, and so also sidesteps the permanent negative caching in
`ViewportWidget::texture_for` (`editor/src/viewport/viewport_widget.cpp:175`
caches the miss "so we don't retry"). And the project's overlay manifest must now
be *creatable*: `Project::library_manifest_path()` returns `nullopt` when the file
does not exist and all five of `MainWindow`'s asset-commit writers bail on that,
so today **a fresh project can never author its first asset of any kind** —
crosswalks and prop sets included. `p6-s8` fixes that as a precondition rather
than working around it.

### Project overlays are process-wide, and one pointer contract narrows

The compiled-in catalogues are reached through two lookups that are effectively
global: `props::model()` is a free function over a static table, and
`MaterialCatalog` is constructed ad hoc in eight places — one of them a stack
local inside a free function (`editor/src/document/library_drop.cpp:378`). A
project overlay must be visible from all of them.

**Decision: the overlay is process-wide state, replaced wholesale on project open
and cleared on project close, sitting behind the existing lookup signatures.**
`props::model()` does not change shape at all, so the mesh builder, the scene
builder's batching, prop placement and both exporters are untouched — which is
what makes the roadmap's promise that "scene builder instanced batches key on it
unchanged" true rather than aspirational.

Threading it explicitly instead — one `MaterialCatalog` owned by `Document`, a
registry parameter on the kernel calls — was considered and rejected as a rider:
it is roughly a hundred lines of signature churn across five kernel call sites and
eight editor ones, in a sprint whose actual subject is elsewhere. The cost of the
choice is hidden state, and it is paid down by making the invariant testable: a
project switch replaces the overlay wholesale, and clearing it restores the
built-in catalogue exactly.

**One published contract narrows.** `props::model()`'s doc comment promises a
pointer "valid for the program lifetime (models are static data)". For a
project-backed model that becomes **valid until the project overlay is replaced or
cleared**. This is safe today because nothing caches a `const PropModel*` across a
project switch — `mesh_builder`, `scene_builder`, `prop_placement` and both
exporters all re-resolve per build, and the editor rebuilds mesh and scene on
project change — but it is a real narrowing of a documented guarantee, so it is
stated in the header and pinned by a test rather than left to be discovered.

## Consequences

**Easier.** No dependency review, no `URL_HASH` to pin, no
`THIRD_PARTY_LICENSES.md` row, no build-time cost, no new CI configuration —
`cmake/deps.cmake` is untouched. Flattening keeps `PropPart`, both exporters and
the instanced draw path exactly as they are, so every bundled prop still exports
byte-identically with no regression gate needed for it. The material half needs
**no renderer change whatsoever**: `MaterialDef`'s map fields are already plain
path strings and `texture_for()` already decodes whatever path it is handed, so an
absolute filesystem path flows through a code path built for `qrc:` aliases
without modification.

**Harder.** An imported textured model looks flat, and the user is told so on
every import until [#507](https://github.com/Robomous/RoadMaker/issues/507)
lands — a warning that will be seen often, because textured models are the normal
kind. A project's assets now live in two places conceptually (the user's original
and the project's copy), and the copy is what matters. And the overlay is hidden
global state, which is a real cost however well tested; the honest description is
that this sprint chose consistency with `props::model()`'s existing shape over
inventing a better one halfway.

**Follow-ups this creates** (filed with #322; the flatten warning cites the first
by number):

- [#507](https://github.com/Robomous/RoadMaker/issues/507) — **imported prop
  textures**: UVs and base-colour images through `PropPart`, the batch upload, the
  viewport's per-batch material, and both exporters, with bundled-prop export
  byte-identity as the regression gate. The other half of this record's central
  decision.
- [#508](https://github.com/Robomous/RoadMaker/issues/508) — **a scene using
  project props, opened without its project, drops them with no diagnostic.**
  `core/src/mesh/mesh_builder.cpp:651-655` silently `continue`s on an unresolvable
  `<object @name>`. Nearly harmless while every resolvable name was compiled in;
  a visibility hole once a name can be project-scoped. Mesh building has no
  diagnostic channel at all today, which is why this is a design change rather
  than a one-liner.
- [#509](https://github.com/Robomous/RoadMaker/issues/509) — **render-to-thumbnail
  for imported props.** Explicitly out of scope in #322; imported props fall back
  to the themed glyph the Library already has. Bundled prop thumbnails are
  produced offline by a Python rasteriser, so the editor has no prop previewer to
  reuse.
- [#510](https://github.com/Robomous/RoadMaker/issues/510) — **both lookups are
  linear scans**, fine for 26 props and 5 materials, unmeasured for a project with
  hundreds of imported assets. To be closed by a measurement even if the
  measurement says no change is needed —
  [#502](https://github.com/Robomous/RoadMaker/issues/502) is the standing lesson
  that this kind of cost is found by a bench, not by reading.
- [#511](https://github.com/Robomous/RoadMaker/issues/511) — **OBJ read**, the
  refusal this record's format table cites. Needs a new pinned dependency
  (tinyobjloader, MIT) on its own terms, per Q7.

**Reversal cost, stated plainly.** Low for the format list — a new reader slots in
behind the extension dispatch, and nothing downstream of the importer is
format-aware. Moderate for flattening, and the work is #507's rather than a
reversal: the importer's flatten step is one function, but the representation
change behind it reaches both exporters. Low for the overlay decision in the
kernel (the signature never changed, so threading a registry later is the same
refactor it is today), and moderate in the editor, where eight construction sites
would need to become one.

## References

- [#322](https://github.com/Robomous/RoadMaker/issues/322) — p6-s8, texture→material
  and mesh→prop import pipelines
- [2026-07 realignment](../roadmap/updates/2026-07-realignment.md) — **Q7**, the
  format ruling this record implements (§ Open questions; issue body § C6)
- [ADR-0010](./0010-gis-ingest-bounded-crs.md) — the exact-answer/named-refusal
  principle applied here to textures
- [ADR-0011](./0011-lidar-ingest-in-house-las.md) — the transitive-drag test, which
  this sprint passes trivially by taking nothing
- [ADR-0012](./0012-osm-ingest-xml-in-house.md) — the third consecutive record to
  refuse a format by name and cite the issue that would lift it
- [ADR-0003](./0003-qt-widgets-editor.md) — why `core/` never links Qt, which is
  why a user's image meets two decoders
- [Dependency & licensing policy](../standards/dependencies.md) — the FBX
  prohibition
- [Material system v2](../design/materials-structures/01_material_system.md) — §2,
  the committed `materials[]` manifest schema this sprint finally implements
- glTF 2.0 specification (Khronos); `KHR_materials_pbrMetallicRoughness`
