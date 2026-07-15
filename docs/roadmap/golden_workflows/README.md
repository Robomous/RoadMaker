# Golden workflows — the acceptance mechanism

*Step-by-step scripts a human executes in RoadMaker, each step with an
explicit expected outcome. Passing them by hand is the only acceptance
mechanism of the [Road to Parity roadmap](../README.md).*

A golden workflow is executed **by the maintainer by hand** — automated
replays are complementary evidence, never a substitute. A run passes only
if every step's expected result holds and the editor never crashes.

Specs are written under the
[product-parity and IP rules](../../standards/product-parity.md): RoadMaker
tool names and ASAM OpenDRIVE / OpenSCENARIO vocabulary only.

| ID | Workflow | Exercises | Fed by |
|---|---|---|---|
| [GW-1](gw1_camera.md) | Camera & navigation | Orbit-pivot model, push-past zoom, framing, projections, cardinal views | P1 |
| [GW-2](gw2_simple_scene.md) | Simple scene end-to-end | Roads, auto junction, elevation + bridges, corner radius, crosswalk, road styles, lane carve, markings, props, export previews | P1–P7 |
| [GW-3](gw3_corner_materials.md) | Corner reshaping & materials | Corner control vertices/extents, per-corner and junction materials by drag | P4, P6 |
| [GW-4](gw4_signals.md) | Traffic signals | Auto-signalize templates, linked signal props, Signal Phase Editor | P4 |
| [GW-5](gw5_crosswalk_assets.md) | Parametric crosswalk assets | Library-authored crosswalk assets, parameters, instance overrides | P3, P6 |
| GW-6 | Scenarios (drafted during P8 planning) | Scenario authoring end-to-end | P8 |

## Document format

Each workflow doc contains:

1. **Purpose** — what capability set it accepts.
2. **Preconditions** — build, platform, assets, starting state.
3. **Numbered steps** — each an action followed by its expected result,
   with a checkbox.
4. **Pass criteria** — what must hold overall (always includes
   zero crashes).
5. **Results table** — date, OS, commit, pass/fail, notes; one row per
   hand-executed run. The [release gate](../README.md#release-gate)
   requires a recorded pass on macOS, Linux, and Windows.

Shortcut notation: steps use the macOS binding with the Linux/Windows
equivalent in parentheses, e.g. `⌥ Option (Alt)`.

The pre-reset workflows (first-network, crash-recovery, the v0.4.0 gate)
are preserved in the
[archive](../archive/2026-07-pre-reset/golden_workflows/README.md).
