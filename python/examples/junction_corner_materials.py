"""Author junction materials and a junction-wide corner radius (p4-s2, #226).

Beyond the per-corner fillet geometry of `junction_corner_radius.py`, a
junction carries three more authored slots:

  * `Junction.material` — the carriageway (floor) look, `edit.set_junction_material`.
  * `Junction.default_corner_radius` — the fallback fillet radius every corner
    without its own override uses, `edit.set_junction_default_corner_radius`.
  * `JunctionCorner.sidewalk_material` / `.median_material` — per-corner overlay
    materials, `edit.set_corner_sidewalk_material` / `..._median_material`.

Radius resolution is per-corner override > junction default > derived, and
`JunctionCornerInfo.radius_from_junction_default` says which one won. All of it
persists as `<userData>` on `<junction>` — `rm:junction` for the junction-wide
fields, `rm:corners` for the per-corner ones — because ASAM OpenDRIVE 1.9.0
§12.10 gives `<boundary>` no material or radius carrier.

Run:  python junction_corner_materials.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm

assembly = rm.edit.assembly


def describe(network: rm.RoadNetwork, junction: rm.JunctionId, title: str) -> None:
    print(title)
    junction_data = network.junction(junction)
    print(
        f"  junction: material={junction_data.material or '(derived)'!r} "
        f"default_corner_radius={junction_data.default_corner_radius}"
    )
    for index, corner in enumerate(rm.junction_corners(network, junction)):
        if corner.radius_authored:
            source = "per-corner"
        elif corner.radius_from_junction_default:
            source = "jct-default"
        else:
            source = "derived    "
        print(
            f"  corner {index}: radius={corner.radius:6.2f} m from {source} "
            f"(max {corner.max_radius:5.2f})  sidewalk="
            f"{corner.sidewalk_material or '-':>10}  median={corner.median_material or '-':>10}"
        )


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_corner_materials.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A roomy 4-way crossing: the generous gap leaves the arm faces enough room
    # that an authored radius is not immediately clamped to `max_radius`.
    params = assembly.IntersectionParams()
    params.gap_m = 24.0
    stack.push(network, assembly.x_intersection(network, assembly.Pose(0.0, 0.0, 0.0), params))
    junction = network.junction_ids[0]

    describe(network, junction, "derived junction (nothing authored):")

    # 1. One junction-wide radius for every corner at once. It is stored
    #    uncapped and clamped to each corner's geometry only at solve time.
    stack.push(network, rm.edit.set_junction_default_corner_radius(network, junction, 7.5))
    describe(network, junction, "\nafter set_junction_default_corner_radius(7.5):")

    # 2. The carriageway material — a bare catalog name, like Surface.material.
    stack.push(network, rm.edit.set_junction_material(network, junction, "cobblestone"))

    # 3. Per-corner overlay materials on the first corner. The corner is named
    #    by its adjacent arm pair, so the identity survives a regenerate.
    first = rm.junction_corners(network, junction)[0]
    stack.push(
        network,
        rm.edit.set_corner_sidewalk_material(
            network, junction, first.arm_a, first.arm_b, "concrete"
        ),
    )
    stack.push(
        network,
        rm.edit.set_corner_median_material(network, junction, first.arm_a, first.arm_b, "grass"),
    )
    describe(network, junction, "\nafter the junction material + corner 0 overlays:")

    # 4. A per-corner radius BEATS the junction-wide default — and only on its
    #    own corner; the others keep falling back to the default.
    stack.push(
        network, rm.edit.set_corner_radius(network, junction, first.arm_a, first.arm_b, 4.0)
    )
    describe(network, junction, "\nafter set_corner_radius(4.0) on corner 0 (per-corner wins):")
    solved = rm.junction_corners(network, junction)
    assert solved[0].radius_authored and not solved[0].radius_from_junction_default
    assert all(info.radius_from_junction_default for info in solved[1:])

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"\nvalidation: {len(findings)} findings, {len(errors)} errors")
    assert not errors, "authored materials must not break OpenDRIVE validity"

    # 5. Persist, then read it all back.
    rm.save_xodr(network, out_path, "junction_corner_materials_example")
    reloaded, diagnostics = rm.load_xodr(out_path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR], diagnostics

    reloaded_junction = reloaded.junction(reloaded.junction_ids[0])
    assert reloaded_junction.material == "cobblestone"
    assert reloaded_junction.default_corner_radius == 7.5
    override = reloaded_junction.corners[0]
    assert override.sidewalk_material == "concrete"
    assert override.median_material == "grass"
    assert override.radius == 4.0
    describe(reloaded, reloaded.junction_ids[0], f"\nreloaded from {out_path}:")

    # 6. Walk the undo chain all the way back — the network returns to the
    #    fully derived junction it started as.
    while stack.can_undo:
        stack.undo(network)
    assert network.junction_ids == []
    print("\nundid every command: the network is empty again")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
