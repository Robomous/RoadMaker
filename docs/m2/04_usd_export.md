# M2 OpenUSD export — spike results and decision record

Status: **decided 2026-07-10** (maintainer sign-off) — **Option A: tinyusdz,
USDA-only in M2.** Implementation: phase 5 (issue #20), behind `RM_BUILD_USD`
(OFF stub landed with the plan batch).

## Context

M2 adds OpenUSD export (meshes, UsdPreviewSurface materials, Road→LaneSection→
Lane hierarchy, Y-up/meters via the shared exporter stage factored out of
`gltf_exporter`). Two candidate strategies were spiked, time-boxed, on
2026-07-10:

- **Option A — tinyusdz** (lighttransport/tinyusdz, Apache-2.0): lightweight,
  FetchContent-friendly.
- **Option B — Pixar OpenUSD minimal build**: heavyweight, first-class.

## Spike method (Option A)

Standalone CMake project (outside the repo), FetchContent-pinned
**tinyusdz v0.9.1** (commit `a04ee0bc`, release tarball sha256
`7e3d6dd8…33e2` — full hash in the pin when it lands in `cmake/deps.cmake`).
Authored in-memory: 2 roads × lane-section Xforms × 2–3 lane `GeomMesh`es,
stage metadata `upAxis=Y`, `metersPerUnit=1.0`, `defaultPrim`, 3 materials
(`lane_0`, `lane_1`, `lane_marking`) as UsdPreviewSurface with distinct
`diffuseColor`, bound per mesh. Validated with `usdchecker` (pxr usd-core
26.5 from PyPI) and a 20-prim deep-compare round-trip. Blender was not
installed on the dev machine; validation relied on pxr's ComplianceChecker
(the reference implementation).

## Gate results

| Gate | Result | Evidence |
|---|---|---|
| G1 valid USDA (usdchecker) | **PASS** | 0 errors / 0 failed checks / 0 warnings — after authoring `prepend apiSchemas = ["MaterialBindingAPI"]` per bound mesh (tinyusdz can express it; nothing applies it for you — implementation note for the exporter) |
| G2 valid USDC (crate) | **FAIL (hard)** | `usdc::SaveAsUSDCToFile` is an unimplemented stub (`src/usdc-writer.cc:556`), same on release-branch HEAD; last touched 2024-10. No USDZ writer either. Not fixable by pinning newer |
| G3 PreviewSurface materials + bindings | **PASS** (w/ caveat) | Bindings resolve via `ComputeBoundMaterial()`; semantic round-trip through crate is identical — but the crate leg was written by pxr, not tinyusdz. Texture/shader networks untested (tinyusdz TODO) — not needed for M2's flat materials |
| G4 metadata + hierarchy | **PASS** | upAxis/metersPerUnit/defaultPrim + 3-level Xform nesting read back exactly |
| G5 build weight | **PASS** | 51 s clean build (-j8), 13 MB static lib, 1.9 MB exe, zero transitive deps. Warts: requires `TINYUSDZ_NO_WERROR=ON` (not warning-clean on AppleClang 21); reader modules can't be compiled out (export-only still ships readers); no INTERFACE include dirs (hand-rolled target, same pattern as other deps) |
| G6 license policy | **PASS** (2 flags) | Apache-2.0 core; vendored compiled-in code all permissive. Flags: **mapbox/eternal (ISC)** and **linalg.h (Unlicense)** are outside the enumerated MIT/BSD/Apache-2.0/MPL-2.0/zlib/BSL-1.0 set — **maintainer approved 2026-07-10** (both more permissive than MIT in substance). jsteemann/atoi ships no license text in-tree (upstream Apache-2.0) — record in THIRD_PARTY_LICENSES.md with the upstream reference |

## Option B assessment (triggered by G2)

OpenUSD v26.05 (2026-04): boost is gone; core needs only CMake + oneTBB
(Apache-2.0). License is "Tomorrow Open Source Technology License 1.0" —
Apache-2.0 with a modified trademarks section (would need its own
THIRD_PARTY review). Export-only configuration
(`PXR_BUILD_IMAGING=OFF`, `PXR_ENABLE_PYTHON_SUPPORT=OFF`,
`PXR_BUILD_USD_TOOLS=OFF`, `PXR_ENABLE_GL_SUPPORT=OFF`): est. 10–20 min
build, low-hundreds-MB install, **shared-lib + plugInfo packaging on all
three platforms and in every installer** — the dominant cost.

## Decision

**tinyusdz v0.9.1, USDA (ASCII) export only, for M2.** Rationale:

- Every relevant consumer (usdview, Omniverse/Isaac Sim, Blender) reads
  `.usda`; the loss is file size / load speed on large networks, acceptable at
  M2's target scale.
- Dependency-policy fit is a core project value: one pinned archive, zero
  transitive deps, 51 s build — vs a packaging surface (OpenUSD shared libs +
  plugInfo in DMG/NSIS/AppImage) that would dwarf the feature.
- The two license flags were reviewed and approved by the maintainer.

Consequences and follow-ups:

- `.usdc`/`.usdz` are a **documented limitation** (README + export dialog
  tooltip). Revisit OpenUSD in M3+ if users need crate; the exporter is
  written against our own intermediate stage-model so the backend can swap.
- Exporter must apply `MaterialBindingAPI` apiSchemas explicitly (G1 lesson).
- Pin lands in `cmake/deps.cmake` with URL+SHA256 + THIRD_PARTY_LICENSES.md
  rows (tinyusdz + vendored flags) in the same commit, per policy.
- `RM_BUILD_USD` gates the exporter; CI adds one Linux job with it ON;
  golden-file gtests parse exported USDA textually; `usdchecker` runs in that
  CI job (usd-core via pip, cached venv).
- Wheels stay USD-off in M2 (unchanged from the roadmap).
