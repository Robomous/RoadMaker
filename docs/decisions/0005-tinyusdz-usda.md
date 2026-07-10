# ADR-0005: OpenUSD export via tinyusdz, USDA-only in M2

*Why USD export ships on tinyusdz writing ASCII `.usda` only, instead of a
Pixar OpenUSD build — and what that defers.*

- **Status:** accepted
- **Date:** 2026-07-10
- **Deciders:** Armando Anaya

## Context

M2 adds OpenUSD export (meshes, UsdPreviewSurface materials,
Road→LaneSection→Lane hierarchy, Y-up/meters). Two strategies were spiked on
2026-07-10 — full method, gate table, and evidence in the
[USD export decision record](../design/m2/04_usd_export.md) and the
[spike report](../design/m2/04a_usd_spike_report.md):

- **tinyusdz** (Apache-2.0): one pinned archive, zero transitive
  dependencies, ~51 s clean build — but its binary crate (`.usdc`) writer is
  an unimplemented stub upstream, so it can write ASCII USDA only.
- **Pixar OpenUSD** (minimal build): first-class and writes every format,
  but costs a 10–20 minute build, a low-hundreds-MB install, and shared-lib
  + `plugInfo` packaging inside every installer on three platforms.

## Decision

**tinyusdz v0.9.1, USDA (ASCII) export only, behind `RM_BUILD_USD`
(OFF by default).** Every relevant consumer (usdview, Omniverse/Isaac Sim,
Blender) reads `.usda`; the loss is file size and load speed at scales
beyond M2's targets. The exporter is written against RoadMaker's own
intermediate stage model so the backend can be swapped without touching
callers.

License due diligence: tinyusdz vendors mapbox/eternal (ISC) and linalg.h
(Unlicense) — both outside the enumerated allowed set, both more permissive
than MIT in substance, **maintainer-approved 2026-07-10** and recorded in
`THIRD_PARTY_LICENSES.md`.

## Consequences

- `.usdc`/`.usdz` output is a documented limitation (README + export dialog).
  Revisit Pixar OpenUSD in M3+ if users need crate output.
- The exporter must apply `MaterialBindingAPI` apiSchemas explicitly —
  tinyusdz can express but does not add them (spike gate G1 lesson).
- CI runs one Linux job with `RM_BUILD_USD=ON`; exported USDA is validated
  with golden-file tests and `usdchecker` (usd-core from PyPI).
- Wheels stay USD-off in M2.

## References

- [M2 USD export decision record](../design/m2/04_usd_export.md)
- [USD spike report](../design/m2/04a_usd_spike_report.md)
- [Dependency & licensing policy](../standards/dependencies.md)
