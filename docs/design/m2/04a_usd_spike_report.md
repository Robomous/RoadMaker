# Spike: tinyusdz for RoadMaker OpenUSD export-only

Date: 2026-07-10 · Machine: macOS (Darwin 25.5.0), Intel i9-9880H 8c/16t,
AppleClang 21.0.0, CMake 4.3.4, Ninja 1.13.2
Validator: pip `usd-core` 26.5 (pxr Usd 0.26.5) in a throwaway venv.
Working dir: this folder (`usd-spike/`). RoadMaker repo untouched.

## Verdict

**tinyusdz v0.9.1 FAILS gate G2: it cannot write .usdc (crate binary) at
all.** `tinyusdz::usdc::SaveAsUSDCToFile` is a stub that returns
"USDC writer is not yet implemented." — in the v0.9.1 release *and* on the
current default-branch HEAD (the stub was last touched Oct 2024). There is
also no USDZ writer (USDZ is a zip of usdc, so it is blocked on the same
stub).

Everything else passes: tinyusdz's low-level scene-construction API can
author the full RoadMaker export shape (nested Xform/Mesh hierarchy,
float3 points/normals, int indices, UsdPreviewSurface materials with
bindings and `MaterialBindingAPI` apiSchemas, upAxis=Y, metersPerUnit=1),
and the resulting .usda passes pxr's compliance checker with zero errors,
zero failed checks, zero warnings.

**Recommendation:** tinyusdz is sufficient **only if .usda (ASCII) output is
an acceptable v1 deliverable**. If binary .usdc/.usdz is a requirement (it
usually is for interchange with commercial road-authoring pipelines, Omniverse,
and AR consumers), tinyusdz alone does not meet the bar today; see the
Option B assessment at the end.

---

## Pinned version under test

| Item | Value |
| --- | --- |
| Repo | https://github.com/lighttransport/tinyusdz |
| Tag | `v0.9.1` (latest release, published 2025-11-04; releases before it: v0.9.0 2025-06-27, v0.8.0rc8 2024-07-08) |
| Commit | `a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb` |
| Tarball | https://github.com/lighttransport/tinyusdz/archive/refs/tags/v0.9.1.tar.gz |
| SHA-256 | `7e3d6dd8f54bfa8c7afe830d4505f7740bc26d5055f5f2a603ee9585872933e2` (20 MB) |
| License | Apache-2.0 (top-level LICENSE; GitHub reports NOASSERTION because the file has a preamble) |

Note on "rolling dev tags": the repo's default branch is `release` and is
active (pushed 2026-07-10), but v0.9.1 is a real, recent release tag, so it
is the correct pin. The G2-blocking stub is identical on `release` HEAD, so
pinning newer would not change the verdict.

## What was built

Standalone project in `spike/` (CMakeLists.txt + main.cc), FetchContent
pinned to the tag+hash above, minimal export-only feature set (Tydra,
MaterialX, audio, image loaders, obj/fbx/vox importers, C API, pxr-compat
API all OFF). `spike/main.cc` constructs the stage fully in memory:

```
/RoadNetwork                      (Xform, defaultPrim)
  /Materials                      (Scope)
    /lane_0, /lane_1, /lane_marking   (Material + Shader "PreviewSurface",
                                       UsdPreviewSurface, distinct diffuseColor)
  /Road_1, /Road_2                (Xform, double3 xformOp:translate)
    /LaneSection_0                (Xform)
      /Lane_neg1 /Lane_neg2 /Lane_pos1 /Marking_center   (Mesh, 2 tris each,
        point3f[] points, normal3f[] normals (vertex interp), int[] indices,
        rel material:binding, prepend apiSchemas=["MaterialBindingAPI"])
```

Kernel Z-up → USD Y-up applied at the boundary as (x, y, z) → (x, z, −y),
mirroring the glTF exporter convention. Lane naming follows OpenDRIVE ids
(neg = right of reference line).

Commands:

```
cmake -S spike -B spike/build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build spike/build -j 8
./spike/build/usd_spike        # writes roadnetwork.usda, attempts roadnetwork.usdc
python3 -m venv venv && ./venv/bin/pip install usd-core
./venv/bin/python validate.py spike/build/roadnetwork.usda
./venv/bin/python roundtrip.py spike/build/roadnetwork.usda spike/build/roadnetwork_pxr.usdc
```

---

## Gates

### G1 — valid USDA accepted by usdchecker: **PASS**

The `usd-core` pip wheel ships no CLI scripts, so the check runs
`pxr.UsdUtils.ComplianceChecker` directly (`validate.py`) — this is the
exact engine the `usdchecker` CLI historically wrapped (pxr now flags it
deprecated in favor of the Usd Validation Framework; the checks executed
are the same suite).

Final result on `spike/build/roadnetwork.usda`:

```
errors: NONE
failedChecks: NONE
warnings: NONE
CHECK RESULT: PASS
```

Evidence trail: the first run FAILED with 8×
`MaterialBindingAPIAppliedChecker` ("Found material bindings but no
MaterialBindingAPI applied on the prim"). Fixed by authoring
`prepend apiSchemas = ["MaterialBindingAPI"]` via
`Prim::metas().apiSchemas` (tinyusdz `APISchemas` struct,
`ListEditQual::Prepend`). tinyusdz *can* express this, but nothing in its
API does it for you — the exporter must know the rule. (tinyusdz's own
api_tutorial carries a TODO for exactly this.)

### G2 — valid USDC accepted by usdchecker: **FAIL (hard)**

```
$ ./usd_spike
Wrote to [roadnetwork.usda]
SaveAsUSDCToFile failed: USDC writer is not yet implemented.
```

- `src/usdc-writer.cc:556` in v0.9.1: `(*err) += "USDC writer is not yet implemented.\n";`
- Same line present on `release` branch HEAD (fetched 2026-07-10).
- `git log` for that file: last functional commit 2024-10-03 ("Fix
  compilation when disabling USDC writer") — no implementation activity.
- No `SaveAsUSDZ` / usdz-writer API exists anywhere in `src/`.

No .usdc file was produced; nothing to run usdchecker on. This gate cannot
be satisfied by tinyusdz today.

### G3 — UsdPreviewSurface materials + bindings expressible, survive round-trip: **PASS (with caveats)**

Expressible, via the low-level structs only (there is no high-level
"scene construction API" — tinyusdz's own examples say so):

- `tinyusdz::Material` + `tinyusdz::Shader` (name "PreviewSurface",
  `info_id = kUsdPreviewSurface`) + `tinyusdz::UsdPreviewSurface` value
  struct with typed fields (`diffuseColor`, `roughness`, `metallic`,
  `useSpecularWorkflow`), `outputsSurface.set_authored(true)`.
- Material output connection authored manually as a `Path` to
  `<mat>/PreviewSurface.outputs:surface` (no connection-builder API).
- Binding: `GeomMesh::materialBinding` relationship + manual
  `apiSchemas` metadata (see G1).

pxr reads it all back correctly (validate.py):

```
materials: ['/RoadNetwork/Materials/lane_0', '/RoadNetwork/Materials/lane_1',
            '/RoadNetwork/Materials/lane_marking']
/RoadNetwork/Materials/lane_0: surface <- .../PreviewSurface id=UsdPreviewSurface diffuseColor=(0.2, 0.2, 0.22)
/RoadNetwork/Materials/lane_1: ... diffuseColor=(0.28, 0.28, 0.3)
/RoadNetwork/Materials/lane_marking: ... diffuseColor=(0.95, 0.95, 0.9)
8 mesh prims, every ComputeBoundMaterial() resolves to the intended material
```

usdcat-style round-trip (roundtrip.py): tinyusdz .usda → pxr crate export
→ reopen → deep compare of stage metadata, prim types, points, normals,
indices, counts, computed bindings, shader ids, diffuseColors:

```
usdc errors: NONE / usdc failedChecks: NONE
usda meta == usdc meta: ('Y', 1.0, '/RoadNetwork')
round-trip integrity: OK (20 prims identical)
```

Caveats: (a) the crate leg is necessarily written by pxr, since tinyusdz
can't (G2); (b) not expressible/tested here: UsdUVTexture / primvar-reader
shader networks (tinyusdz TODO comment in its own tutorial), so texture-based
materials would need verification if ever needed.

### G4 — stage metadata + nested hierarchy: **PASS**

Authored via `stage.metas().upAxis = Axis::Y`, `.metersPerUnit = 1.0`,
`.defaultPrim = token("RoadNetwork")`. pxr reads back:

```
upAxis: Y
metersPerUnit: 1.0
defaultPrim: /RoadNetwork
```

Three-level Xform nesting (RoadNetwork/Road_i/LaneSection_0) with a
`double3 xformOp:translate` + `xformOpOrder` on each road, Mesh leaves —
all round-trip exactly (20/20 prims identical, G3 output). Full USDA at
`spike/build/roadnetwork.usda`.

### G5 — build weight: **PASS**

Clean, cold, ccache disabled, Release, `-j 8`:

| Metric | Value |
| --- | --- |
| Configure (incl. 20 MB tarball download) | 20.3 s wall |
| Build | **51.2 s wall** (309 s user, 44 C++ TUs + lz4.c) |
| `libtinyusdz_static.a` | 13 MB |
| Spike executable (stage construction + USDA write, statically linked) | 1.9 MB |
| Build tree total | 111 MB (includes the 20 MB extracted source) |
| External/transitive deps to provision | **none** — everything vendored, plain C++17 |

Integration warts found (all reproducible from `spike/build.log` history):

1. **Not warning-clean under its own -Werror** with AppleClang 21:
   `-Wmissing-prototypes` errors in `ascii-parser.cc`. Consumers must set
   `TINYUSDZ_NO_WERROR=ON`.
2. **`TINYUSDZ_WITH_MODULE_USDA_READER/USDC_READER=OFF` does not compile**
   (bit-rotted stubs: undeclared identifiers in `ascii-parser.cc:5300`,
   mismatched out-of-line definitions in `usda-reader.cc:1784`,
   `usdc-reader.cc:3927`). Readers must be compiled even for export-only
   use — the 51 s / 13 MB figures already include them.
3. **`tinyusdz_static` exports no INTERFACE include directories**; the
   consumer must add `${tinyusdz_SOURCE_DIR}/src` manually.
4. 17 compiler warnings remain in the build with warnings enabled.

### G6 — license policy fit: **PASS with two flags for maintainer sign-off**

Top-level: Apache-2.0 (allowed). Bundled third-party actually compiled
into the minimal export-only configuration (verified from compiled TU list
+ include analysis — the big items in `src/external/` such as OpenFBX,
stb, tinyexr, wuffs, dr_mp3/dr_wav, nanobind/pybind11, pugixml are
**not compiled** with our flags):

| Component (compiled in) | License | Allowed set? |
| --- | --- | --- |
| lz4 (`src/lz4/`) | BSD-2-Clause | yes |
| fast_float | Apache-2.0 / MIT / BSL-1.0 (choice) | yes |
| floaxie | Apache-2.0 | yes |
| jsteemann/atoi | Apache-2.0 (upstream SPDX; **no license text bundled** in tinyusdz) | yes (flag the missing text) |
| dtoa_milo | MIT | yes |
| jeaiii itoa | MIT | yes |
| ghc filesystem | MIT | yes |
| foonathan string_id | zlib | yes |
| simple_match | BSL-1.0 | yes |
| **mapbox/eternal** | **ISC** | **not in allowed list** — flag |
| **linalg.h** | **Unlicense** (public domain) | **not in allowed list** — flag |

ISC and Unlicense are both maximally permissive (ISC ≈ simplified MIT;
Unlicense = public-domain dedication) and MIT-compatible, but the
RoadMaker policy enumerates MIT/BSD/Apache-2.0/MPL-2.0/zlib/BSL-1.0 only,
so per policy these two require explicit maintainer approval before
adoption. Nothing GPL/LGPL/proprietary anywhere in the compiled set. If
adopted, THIRD_PARTY_LICENSES.md would need rows for tinyusdz **and** each
compiled-in vendored component above.

### Blender cross-check: **not available**

No `/Applications/Blender.app`, no `blender` on PATH, Homebrew, or
Spotlight (`mdfind` bundle-id query empty). Skipped per instructions;
validation relies on usd-core 26.5, which is the reference implementation
anyway.

---

## Step 6 — Option B paper assessment (Pixar OpenUSD minimal build)

Triggered by the G2 failure. Time-boxed, not built. Facts verified against
the OpenUSD repo on 2026-07-10:

- **Current release:** v26.05 (2026-04-24). Previous: v26.03, v25.11.
- **License:** "Tomorrow Open Source Technology License 1.0" = Apache-2.0
  with a modified Section 6 (Trademarks) (LICENSE.txt, `release` branch).
  Effectively Apache-2.0-grade for our purposes but *not* stock
  Apache-2.0 — needs a maintainer nod and its own THIRD_PARTY row.
- **Boost: no longer required.** README.md (release branch) lists core
  requirements as exactly: C/C++ compiler, CMake, **Intel TBB** — no
  boost. Historical boost usage in core was removed across 24.xx; the last
  remnant (deprecated `boost::python` option) was deleted in v25.05
  (CHANGELOG: "Removed deprecated boost::python support... internal
  pxr_boost::python"), and that only ever mattered for Python bindings,
  which an export-only build disables anyway.
- **Minimal export-only configure** (no imaging, no python, no tools):
  `PXR_BUILD_IMAGING=OFF PXR_ENABLE_PYTHON_SUPPORT=OFF PXR_BUILD_TESTS=OFF
  PXR_BUILD_EXAMPLES=OFF PXR_BUILD_TUTORIALS=OFF PXR_BUILD_USD_TOOLS=OFF
  PXR_ENABLE_GL_SUPPORT=OFF` (+ optionally `PXR_BUILD_MONOLITHIC=ON` for a
  single `libusd_ms`). This drops OpenSubdiv/OpenEXR/OIIO/OCIO/MaterialX
  and every GUI dep; the only third-party build requirement left is
  **oneTBB (Apache-2.0, allowed)**.
- **Estimated weight** (not measured): USD core is roughly 40–60× the TU
  count of tinyusdz; expect ~10–20 min clean build on this 8-core machine
  and an install in the low hundreds of MB, producing ~20 shared libs (or
  one monolithic lib ~60–100 MB). Runtime deployment must ship the lib(s)
  plus `plugInfo.json` plugin metadata — heavier than tinyusdz in every
  dimension, but it is the reference crate writer: .usda, .usdc, .usdz all
  first-class, `usdchecker`-clean by construction.
- **Risks vs RoadMaker constraints:** plugin-registry layout complicates
  static embedding into the kernel (shared libs are the supported path,
  fine license-wise since it's Apache-family, but it means editor-side
  packaging work on all 3 platforms); Windows MSVC builds are supported
  first-class by Pixar CI.

## Decision options for the record

1. **USDA-only v1 with tinyusdz** — smallest footprint (51 s build, 13 MB
   static lib, zero transitive deps), passes G1/G3/G4/G5/G6(flagged);
   accept ASCII-only export and document it. Crate/usdz deferred.
2. **OpenUSD core minimal build** — full .usdc/.usdz correctness, ~2
   orders of magnitude more build/deploy weight, TBB dep, plugin packaging
   work; license needs "modified Apache" sign-off.
3. Hybrid: ship USDA now via tinyusdz; revisit crate when tinyusdz's
   writer lands (no sign of active work — do not plan on it) or when a
   milestone justifies Option 2.

## Artifacts in this folder

- `spike/CMakeLists.txt`, `spike/main.cc` — the pinned FetchContent project
- `spike/build/roadnetwork.usda` — tinyusdz output (usdchecker-clean)
- `spike/build/roadnetwork_pxr.usdc` — pxr-written crate used for round-trip
- `spike/configure.time`, `spike/build.time`, `spike/build.log` — timings/logs
- `validate.py`, `roundtrip.py` — usd-core validation scripts
- `venv/` — throwaway usd-core 26.5 venv
- `tinyusdz-v0.9.1.tar.gz` + extracted `tinyusdz-0.9.1/` — audited source
