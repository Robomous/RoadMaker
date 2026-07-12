#!/usr/bin/env python3
"""GW-1 "First network" — headless replay of the automatable steps.

Drives the kernel command layer (roadmaker.edit.EditStack — the same
command layer the editor's undo/redo uses) through the action script of
docs/roadmap/golden_workflows/gw1_first_network.md, asserting each step's
observable outcome. Interactive-only aspects (mouse UX, panel rendering)
and external tools (esmini, USD) are out of scope here and recorded as
such in the gate document; this replay is the pre-flight evidence, not a
substitute for the maintainer's by-hand gate run.

Run:  python3 scripts/gw1_replay.py [output-dir]
Exit code 0 = every automatable step passed.
"""

from __future__ import annotations

import pathlib
import sys

import roadmaker as rm

MAX_GRADE = 0.12          # editor default (WriterOptions::max_grade_warning)
CLEARANCE_TARGET = 5.5    # editor default overpass clearance, metres
GW1_MIN_CLEARANCE = 5.0   # the workflow's pass bar


def created_road(network: rm.RoadNetwork, stack: rm.edit.EditStack, command) -> rm.RoadId:
    """Push a create_road command and return the id it created."""
    before = set(map(str, network.road_ids))
    stack.push(network, command)
    new = [r for r in network.road_ids if str(r) not in before]
    assert len(new) == 1, f"expected exactly one new road, got {new}"
    return new[0]


def elevation_z(road: rm.Road, s: float) -> float:
    """z(s) from the road's <elevation> records (0 when no profile)."""
    z = 0.0
    for record in road.elevation:
        if record.s <= s:
            z = record.eval(s)
    return z


def plan_crossing(a: rm.Road, b: rm.Road) -> tuple[float, float, float]:
    """(s_on_a, s_on_b, distance) of the closest plan-view approach."""

    def sweep(sa0, sa1, sb0, sb1, step):
        best = (sa0, sb0, float("inf"))
        sb = sb0
        while sb <= sb1:
            pb = b.plan_view.evaluate(sb)
            sa = sa0
            while sa <= sa1:
                pa = a.plan_view.evaluate(sa)
                d = ((pa.x - pb.x) ** 2 + (pa.y - pb.y) ** 2) ** 0.5
                if d < best[2]:
                    best = (sa, sb, d)
                sa += step
            sb += step
        return best

    sa, sb, _ = sweep(0.0, a.plan_view.length, 0.0, b.plan_view.length, 1.0)
    return sweep(max(0.0, sa - 2.0), min(a.plan_view.length, sa + 2.0),
                 max(0.0, sb - 2.0), min(b.plan_view.length, sb + 2.0), 0.05)


def crossing_partner(network: rm.RoadNetwork, over: rm.RoadId) -> tuple[rm.Road, float, float, float]:
    """The non-connecting road the overpass road actually crosses."""
    best = None
    for rid in network.road_ids:
        road = network.road(rid)
        if str(rid) == str(over) or network.junction(road.junction) is not None:
            continue
        s_road, s_over, gap = plan_crossing(road, network.road(over))
        if best is None or gap < best[3]:
            best = (road, s_road, s_over, gap)
    assert best is not None
    return best


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("gw1_replay_out")
    out_dir.mkdir(parents=True, exist_ok=True)

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    results: list[tuple[str, str]] = []

    # Step 1 — two-lane road, 4+ waypoints, visible curvature.
    main_road = created_road(
        network, stack,
        rm.edit.create_road(
            [(-150.0, 0.0), (-60.0, 25.0), (0.0, 0.0), (60.0, -25.0), (150.0, 0.0)],
            rm.LaneProfile.two_lane_default(), "GW-1 main"),
    )
    waypoints = rm.edit.effective_waypoints(network.road(main_road))
    assert len(waypoints) >= 4
    curvatures = [abs(network.road(main_road).plan_view.evaluate(s).curvature)
                  for s in rm.edit.waypoint_stations(network.road(main_road))]
    assert max(curvatures) > 1e-3, "road has no visible curvature"
    results.append(("1. Two-lane road, 4+ waypoints, curves",
                    f"{len(waypoints)} waypoints, max |curvature| {max(curvatures):.4f} 1/m"))

    # Step 2 — T against road 1: endpoint attaches to road 1's SIDE via the
    # same snap query the editor's side-snap indicator uses.
    side_road = created_road(
        network, stack,
        rm.edit.create_road([(0.0, -60.0), (0.0, -12.0)],
                            rm.LaneProfile.two_lane_default(), "GW-1 side"),
    )
    snap = rm.edit.snap_to_road_side(
        network, (0.0, -12.0), rm.edit.SnapOptions(radius=20.0, exclude_road=side_road))
    assert snap is not None and str(snap.road) == str(main_road), "side-snap missed road 1"
    stack.push(network, rm.edit.attach_t_junction(
        network, rm.RoadEnd(side_road, rm.ContactPoint.END), snap.road, snap.s))
    assert network.junction_count == 1
    junction = network.junction_ids[0]
    arm_count = len(network.junction(junction).arms)
    results.append(("2. T against road 1 (side attach)",
                    f"snap s={snap.s:.2f} d={snap.distance:.2f} m; 1 junction, {arm_count} arms, "
                    f"{len(network.junction(junction).connections)} connections"))

    # Step 3 — overpass: a third road crosses road 1 with NO junction; cross
    # over with clearance >= 5 m (editor default target 5.5 m).
    over_road = created_road(
        network, stack,
        rm.edit.create_road([(80.0, -160.0), (80.0, 140.0)],
                            rm.LaneProfile.two_lane_default(), "GW-1 overpass"),
    )
    crossed, s_main, s_over, gap = crossing_partner(network, over_road)
    assert gap < 0.5, f"roads do not cross in plan view (min gap {gap:.2f} m)"
    crest_z = elevation_z(crossed, s_main) + 6.0
    # Hermite peak grade = 1.5x average; 0.9 keeps the peak clear of the
    # advisory threshold instead of exactly on it.
    ramp = max(20.0, 1.5 * crest_z / (MAX_GRADE * 0.9))
    length = network.road(over_road).length
    assert ramp < s_over < length - ramp, "overpass road too short for the ramps"
    stack.push(network, rm.edit.set_elevation_profile(network, over_road, [
        rm.edit.ElevationPoint(0.0, 0.0, 0.0),
        rm.edit.ElevationPoint(s_over - ramp, 0.0, 0.0),
        rm.edit.ElevationPoint(s_over, crest_z, 0.0),
        rm.edit.ElevationPoint(s_over + ramp, 0.0, 0.0),
        rm.edit.ElevationPoint(length, 0.0, 0.0),
    ]))
    clearance = elevation_z(network.road(over_road), s_over) - elevation_z(crossed, s_main)
    assert clearance >= GW1_MIN_CLEARANCE, f"clearance {clearance:.2f} m < {GW1_MIN_CLEARANCE}"
    assert network.junction_count == 1, "overpass must not create a junction"
    results.append(("3. Overpass, clearance >= 5 m",
                    f"clearance {clearance:.2f} m at s={s_over:.1f}; ramps {ramp:.0f} m; no new junction"))

    # Step 4 — lane profile edit: sidewalk on one side of the side road.
    section = network.road(side_road).sections[0]
    lanes_before = len(network.lane_section(section).lanes)
    stack.push(network, rm.edit.add_lane(network, section, -1, rm.LaneType.SIDEWALK))
    sidewalk = network.lane_section(section).lanes[-1]
    stack.push(network, rm.edit.set_lane_width(network, sidewalk, 2.0))
    assert len(network.lane_section(section).lanes) == lanes_before + 1
    results.append(("4. Sidewalk lane added",
                    f"side road lanes {lanes_before} -> {lanes_before + 1}, width 2.0 m"))

    # Step 5 — drag a node of a T-junction incoming road, then regenerate:
    # kernel parity for the editor's junction-adjacent drag (which collapses
    # both into one undo step via its regeneration-aware command push).
    connecting_before = {str(c.connecting_road) if hasattr(c, "connecting_road") else str(c)
                         for c in network.junction(junction).connections}
    stack.push(network, rm.edit.move_waypoint(network, side_road, 0, (-15.0, -58.0)))
    stack.push(network, rm.edit.regenerate_junction(network, junction))
    assert network.junction_count == 1
    assert len(network.junction(junction).connections) == len(connecting_before)
    results.append(("5. Drag junction incoming node -> regen",
                    f"moved side-road start 15 m; junction kept {len(connecting_before)} connections"))

    # Step 6 — undo x10 / redo x10, byte-identical afterwards.
    stack.push(network, rm.edit.rename_road(network, main_road, "Main Street"))
    assert stack.size >= 10, f"only {stack.size} commands recorded"
    before = rm.write_xodr(network, "gw1_replay")
    for _ in range(10):
        stack.undo(network)
    mid = rm.write_xodr(network, "gw1_replay")
    assert mid != before, "undo x10 changed nothing"
    for _ in range(10):
        stack.redo(network)
    after = rm.write_xodr(network, "gw1_replay")
    assert after == before, "redo x10 did not restore the network byte-identically"
    results.append(("6. Undo x10 / redo x10 identical",
                    f"{stack.size} commands; write_xodr byte-identical after redo"))

    # Step 7 — persistence + interchange: save, reload, glTF export. (USD and
    # esmini run in CI / by the maintainer; recorded in the gate document.)
    xodr_path = out_dir / "gw1_network.xodr"
    rm.save_xodr(network, xodr_path, "gw1_replay")
    reloaded, diagnostics = rm.load_xodr(xodr_path)
    load_errors = [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert not load_errors, f"reload errors: {load_errors}"
    assert reloaded.road_count == network.road_count
    assert rm.write_xodr(reloaded, "gw1_replay") == rm.write_xodr(network, "gw1_replay"), \
        "save -> reload -> save is not byte-identical"
    glb_path = out_dir / "gw1_network.glb"
    rm.export_glb(rm.build_network_mesh(reloaded), glb_path)
    assert glb_path.stat().st_size > 0
    results.append(("7. Save/reload; glTF",
                    f"{reloaded.road_count} roads round-trip byte-identical; "
                    f"glb {glb_path.stat().st_size} bytes"))

    # Final diagnostics — GW-1 passes with zero validation ERRORS.
    findings = rm.validate_network(network, target_version=rm.XodrVersion.V1_8_1)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    warnings = [f for f in findings if f.severity == rm.Severity.WARNING]

    print(f"GW-1 replay — roadmaker {rm.version()} — output in {out_dir}/")
    for title, evidence in results:
        print(f"  PASS  {title}: {evidence}")
    print(f"  diagnostics at end: {len(errors)} errors, {len(warnings)} warnings")
    for finding in findings:
        print(f"    {finding}")
    if errors:
        print("FAIL: validation errors present")
        return 1
    print("GW-1 automatable replay: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
