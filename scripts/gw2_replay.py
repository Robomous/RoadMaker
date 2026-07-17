#!/usr/bin/env python3
"""GW-2 "Simple scene end-to-end" — headless replay of the automatable steps.

Drives the kernel command layer (roadmaker.edit.EditStack — the same command
layer the editor's undo/redo uses) through the automatable slice of
docs/roadmap/golden_workflows/gw2_simple_scene.md, asserting each step's
observable outcome. Interactive-only aspects (mouse UX, panel rendering, the
prop techniques, and the export-preview tools) and external tools (esmini) are
out of scope here and recorded as such in the gate document; this replay is the
pre-flight evidence, not a substitute for the maintainer's by-hand gate run.

Official steps mirror the scene doc's numbering: 2 (create road), 3 (crossing
junction), 4 (extend both endpoints), 5 (enclosed-area surface), 11 (road
style), 12 (lane carve), 23 (save/reopen). A cross-cutting undo x10 / redo x10
check mirrors gw1_replay. SUPP-labelled steps exercise the P2 tools the scene's
official steps do not touch (Lane, Lane Width, Lane Add, Lane Form) — they are
printed but EXCLUDED from the official pass tally.

Run:  python3 scripts/gw2_replay.py [output-dir]
Exit code 0 = every official automatable step passed with zero validation
errors. The output .xodr is written to the output dir only (it is NOT a sample
and must never land in assets/samples/).
"""

from __future__ import annotations

import math
import pathlib
import sys

import roadmaker as rm


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


def lane_width(lane: rm.Lane, ds_local: float) -> float:
    """Width w(ds) of a lane at SECTION-LOCAL station ds_local (0 outside)."""
    value = 0.0
    for record in lane.widths:
        if record.s <= ds_local:
            value = record.eval(ds_local)
    return value


def side_lanes(network: rm.RoadNetwork, section, positive: bool) -> list:
    """LaneIds on one side of a section (positive => left, else right)."""
    out = []
    for lane_id in network.lane_section(section).lanes:
        odr = network.lane(lane_id).odr_id
        if (odr > 0) == positive and odr != 0:
            out.append(lane_id)
    return out


def sample_plan(road: rm.Road, s: float) -> tuple[float, float]:
    """(heading, curvature) at station s of a road's plan view."""
    point = road.plan_view.evaluate(s)
    return point.hdg, point.curvature


def hdg_gap(a: float, b: float) -> float:
    """Smallest absolute heading difference [rad], wrapped to [0, pi]."""
    return abs(math.atan2(math.sin(a - b), math.cos(a - b)))


# --------------------------------------------------------------------------- #
# Official steps (mirror gw2_simple_scene.md numbering).                       #
# --------------------------------------------------------------------------- #


def step2_create_road(network, stack) -> tuple[rm.RoadId, str]:
    """Step 2 — a road from 3+ waypoints with the default two-lane profile."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road(
            [(0.0, 0.0), (60.0, 20.0), (120.0, 0.0)],
            rm.LaneProfile.two_lane_default(), "GW-2 main"),
    )
    road = network.road(road_id)
    waypoints = list(road.authoring_waypoints)
    assert len(waypoints) >= 3, f"expected 3+ authoring waypoints, got {len(waypoints)}"
    # authoring_waypoints stay editable — they are the recorded control points,
    # not a geometry re-derivation, so effective_waypoints agrees with them.
    effective = rm.edit.effective_waypoints(road)
    assert len(effective) == len(waypoints), "effective/authoring waypoint mismatch"
    section = road.sections[0]
    left = side_lanes(network, section, True)
    right = side_lanes(network, section, False)
    assert left and right, f"default profile must lane both sides (L={left}, R={right})"
    return road_id, (f"3 authoring waypoints (editable); default profile "
                     f"{len(left)} left / {len(right)} right lane(s)")


def step3_cross(network, stack) -> tuple[rm.JunctionId, str]:
    """Step 3 — a second road across the first forms one junction on commit."""
    road_a = created_road(
        network, stack,
        rm.edit.create_road([(0.0, 40.0), (120.0, 40.0)],
                            rm.LaneProfile.two_lane_default(), "GW-2 A"),
    )
    road_b = created_road(
        network, stack,
        rm.edit.create_road([(60.0, -20.0), (60.0, 100.0)],
                            rm.LaneProfile.two_lane_default(), "GW-2 B"),
    )
    junctions_before = network.junction_count
    stack.push(network, rm.edit.assembly.cross_roads(network, road_a, road_b))
    assert network.junction_count == junctions_before + 1, "cross did not add exactly one junction"
    junction_id = network.junction_ids[-1]
    junction = network.junction(junction_id)
    connections = list(junction.connections)
    assert len(connections) > 0, "junction has no connections"
    # link slots consumed: at least one connection carries per-lane links.
    lane_links = sum(len(c.lane_links) for c in connections)
    assert lane_links > 0, "no per-lane links were consumed"
    return junction_id, (f"1 junction, {len(connections)} connections, "
                         f"{lane_links} lane links consumed")


def step4_extend_both(network, stack) -> str:
    """Step 4 — extend a curved road at BOTH endpoints, continuity at each join."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-220.0, -60.0), (-170.0, -30.0), (-140.0, -60.0)],
                            rm.LaneProfile.two_lane_default(), "GW-2 extend"),
    )

    # END extension: fit continues past the end tangent; the join is at the old
    # length. Sample heading + curvature either side of the join.
    length_before = network.road(road_id).length
    end_point = network.road(road_id).plan_view.evaluate(length_before)
    end_hdg = end_point.hdg
    end_target = (end_point.x + 45.0 * math.cos(end_hdg),
                  end_point.y + 45.0 * math.sin(end_hdg))
    stack.push(network, rm.edit.extend_road(
        network, rm.RoadEnd(road_id, rm.ContactPoint.END), end_target))
    length_after_end = network.road(road_id).length
    before = sample_plan(network.road(road_id), length_before - 0.02)
    after = sample_plan(network.road(road_id), length_before + 0.02)
    end_dh = hdg_gap(before[0], after[0])
    end_dk = abs(before[1] - after[1])
    assert end_dh < 5e-2 and end_dk < 5e-2, f"END join kink dh={end_dh} dk={end_dk}"

    # START extension: prepends the reversed fit and re-bases every s-indexed
    # thing; the old start now sits at s = extension length.
    start_point = network.road(road_id).plan_view.evaluate(0.0)
    start_hdg = start_point.hdg
    start_target = (start_point.x - 45.0 * math.cos(start_hdg),
                    start_point.y - 45.0 * math.sin(start_hdg))
    stack.push(network, rm.edit.extend_road(
        network, rm.RoadEnd(road_id, rm.ContactPoint.START), start_target))
    length_after_start = network.road(road_id).length
    join = length_after_start - length_after_end  # re-base offset = extension length
    before = sample_plan(network.road(road_id), join - 0.02)
    after = sample_plan(network.road(road_id), join + 0.02)
    start_dh = hdg_gap(before[0], after[0])
    start_dk = abs(before[1] - after[1])
    assert start_dh < 5e-2 and start_dk < 5e-2, f"START join kink dh={start_dh} dk={start_dk}"

    return (f"END join dh={end_dh:.1e} rad dk={end_dk:.1e} 1/m; "
            f"START join dh={start_dh:.1e} rad dk={start_dk:.1e} 1/m")


def step5_surface() -> str:
    """Step 5 — an enclosing ring of roads derives one ground surface + mesh."""
    net = rm.RoadNetwork()
    corners = [(0.0, 0.0), (40.0, 0.0), (40.0, 40.0), (0.0, 40.0)]
    for i in range(4):
        rm.author_clothoid_road(
            net, [corners[i], corners[(i + 1) % 4]],
            rm.LaneProfile.two_lane_rural(), "", f"ring{i}")
    assert net.surface_count == 0
    rm.derive_surfaces(net)
    assert net.surface_count >= 1, "enclosed ring derived no surface"
    surface = net.surface(net.surface_ids[0])
    assert surface.source == rm.BoundarySource.DERIVED
    mesh = rm.build_network_mesh(net)
    assert mesh.surface_count >= 1, "surface mesh channel is empty"
    assert mesh.surface_vertex_count > 0, "surface mesh has no vertices"
    return (f"{net.surface_count} surface ({len(surface.bounding_roads)} bounding roads); "
            f"mesh channel {mesh.surface_count} surface(s), "
            f"{mesh.surface_vertex_count} vertices")


def step11_road_style(network, stack) -> str:
    """Step 11 — apply a road style: profile replaced, everything else preserved."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-60.0, 150.0), (20.0, 150.0), (100.0, 150.0)],
                            rm.LaneProfile.two_lane_default(), "Boulevard"),
    )
    length = network.road(road_id).length
    mid = length / 2.0
    stack.push(network, rm.edit.set_elevation_profile(network, road_id, [
        rm.edit.ElevationPoint(0.0, 0.0, 0.0),
        rm.edit.ElevationPoint(length, 3.0, 0.0),
    ]))
    obj = rm.Object()
    obj.name = "tree_pine"
    obj.type = rm.ObjectType.TREE
    obj.s = 10.0
    obj.t = 6.0
    stack.push(network, rm.edit.add_object(network, road_id, obj))

    name_before = network.road(road_id).name
    z_before = elevation_z(network.road(road_id), mid)
    objects_before = len(network.objects_of(road_id))
    pred_before = str(network.road(road_id).predecessor)
    succ_before = str(network.road(road_id).successor)

    style = rm.RoadStyle.urban_two_lane()
    expected_left = len(style.left)
    expected_right = len(style.right)
    stack.push(network, rm.edit.apply_road_style(network, road_id, style))

    road = network.road(road_id)
    assert len(road.sections) == 1, "style must flatten to a single lane section"
    section = road.sections[0]
    got_left = len(side_lanes(network, section, True))
    got_right = len(side_lanes(network, section, False))
    assert got_left == expected_left and got_right == expected_right, (
        f"profile not replaced by style (L {got_left}/{expected_left}, "
        f"R {got_right}/{expected_right})")
    # everything orthogonal to the cross section survives byte-for-byte.
    assert road.name == name_before, "name not preserved"
    assert abs(elevation_z(road, mid) - z_before) < 1e-9, "elevation not preserved"
    assert len(network.objects_of(road_id)) == objects_before, "placed object dropped"
    assert str(road.predecessor) == pred_before, "predecessor link changed"
    assert str(road.successor) == succ_before, "successor link changed"
    return (f"profile -> {got_left}L/{got_right}R in 1 section; "
            f"name/elevation({z_before:.2f} m)/object/links preserved")


def step12_carve(network, stack, carve_road: rm.RoadId) -> str:
    """Step 12 — carve a turn lane: width 0 at s_start, ramps up, holds to end."""
    road = network.road(carve_road)
    length = road.length
    s_start = length * 0.60
    s_end = length * 0.80
    before_final = set(map(str, network.lane_section(road.sections[-1]).lanes))
    stack.push(network, rm.edit.carve_lane(
        network, carve_road, -1, s_start, s_end, -2, rm.LaneType.DRIVING))

    road = network.road(carve_road)
    final_section = road.sections[-1]
    s0 = network.lane_section(final_section).s0
    carved = None
    for lane_id in network.lane_section(final_section).lanes:
        if network.lane(lane_id).odr_id == -2 and str(lane_id) not in before_final:
            carved = network.lane(lane_id)
            break
    assert carved is not None, "carved lane -2 not found in the final section"
    w_start = lane_width(carved, s_start - s0)
    w_end = lane_width(carved, s_end - s0)
    w_term = lane_width(carved, length - s0 - 0.01)
    assert w_start < 5e-2, f"width at s_start not ~0 (={w_start:.3f})"
    assert w_end > 0.5, f"width did not ramp up by s_end (={w_end:.3f})"
    assert abs(w_term - w_end) < 0.1, f"width did not hold to terminus ({w_end:.3f}->{w_term:.3f})"
    return (f"lane -2: w(s_start)={w_start:.3f} -> w(s_end)={w_end:.2f} "
            f"-> w(terminus)={w_term:.2f} m")


def step23_persist(network, out_dir) -> tuple[str, list]:
    """Step 23 — save/reload byte-identical + validate with zero errors."""
    xodr_path = out_dir / "gw2_network.xodr"
    rm.save_xodr(network, xodr_path, "gw2_replay")
    reloaded, diagnostics = rm.load_xodr(xodr_path)
    load_errors = [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert not load_errors, f"reload errors: {load_errors}"
    assert reloaded.road_count == network.road_count, "road count changed on reload"
    assert rm.write_xodr(reloaded, "gw2_replay") == rm.write_xodr(network, "gw2_replay"), \
        "save -> reload -> save is not byte-identical"
    findings = rm.validate_network(network, target_version=rm.XodrVersion.V1_8_1)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    assert not errors, f"validation errors: {errors}"
    return (f"{reloaded.road_count} roads round-trip byte-identical; "
            f"validate: 0 errors"), findings


# --------------------------------------------------------------------------- #
# SUPP steps — P2 tools the official GW-2 steps do not touch.                  #
# --------------------------------------------------------------------------- #


def supp_lane(network, stack) -> str:
    """SUPP Lane — retype a lane and set its travel direction."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-60.0, -150.0), (60.0, -150.0)],
                            rm.LaneProfile.two_lane_default(), "SUPP lane"),
    )
    lane_id = side_lanes(network, network.road(road_id).sections[0], False)[0]
    stack.push(network, rm.edit.set_lane_type(network, lane_id, rm.LaneType.SIDEWALK))
    stack.push(network, rm.edit.set_lane_direction(network, lane_id, rm.LaneDirection.REVERSED))
    lane = network.lane(lane_id)
    assert lane.type == rm.LaneType.SIDEWALK, "lane type not set"
    assert lane.direction == rm.LaneDirection.REVERSED, "lane direction not set"
    # Keep the profile taper on a fresh outer lane so the retype stays clean.
    stack.push(network, rm.edit.set_lane_width_profile(
        network, lane_id, [rm.Poly3(s=0.0, a=0.0, b=0.10, c=0.0, d=0.0)]))
    taper = network.lane(lane_id).widths
    assert taper[0].a == 0.0 and taper[0].b > 0.0, "width taper not applied"
    return "lane retyped SIDEWALK + direction REVERSED; width tapers from 0"


def supp_lane_width(network, stack) -> str:
    """SUPP Lane Width — set_lane_width_profile taper independently."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-60.0, -180.0), (60.0, -180.0)],
                            rm.LaneProfile.two_lane_default(), "SUPP width"),
    )
    lane_id = side_lanes(network, network.road(road_id).sections[0], False)[0]
    stack.push(network, rm.edit.set_lane_width_profile(
        network, lane_id,
        [rm.Poly3(s=0.0, a=3.5, b=0.0, c=0.0, d=0.0),
         rm.Poly3(s=10.0, a=3.5, b=-0.20, c=0.0, d=0.0)]))
    widths = network.lane(lane_id).widths
    assert len(widths) == 2 and widths[1].b < 0.0, "taper profile not applied"
    return f"{len(widths)} width records; second record tapers (b={widths[1].b})"


def supp_lane_add(network, stack) -> str:
    """SUPP Lane Add — add_lane_span pocket lane tapers 0 -> full -> 0."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-60.0, -210.0), (60.0, -210.0)],
                            rm.LaneProfile.two_lane_default(), "SUPP add"),
    )
    length = network.road(road_id).length
    stack.push(network, rm.edit.add_lane_span(
        network, road_id, -1, length * 0.3, length * 0.7, rm.LaneType.DRIVING))
    road = network.road(road_id)
    found = any(
        network.lane(lane_id).odr_id == -2
        for section in road.sections
        for lane_id in network.lane_section(section).lanes)
    assert found, "pocket lane -2 not present after add_lane_span"
    return f"pocket lane -2 present across {len(road.sections)} section(s)"


def supp_lane_form(network, stack) -> str:
    """SUPP Lane Form — form_lane carried across a downstream lane-section seam."""
    road_id = created_road(
        network, stack,
        rm.edit.create_road([(-60.0, -240.0), (60.0, -240.0)],
                            rm.LaneProfile.two_lane_default(), "SUPP form"),
    )
    length = network.road(road_id).length
    seam_s = length * 0.5
    stack.push(network, rm.edit.split_lane_section(network, road_id, seam_s))
    # form_lane starts the lane at 0.25L; the pre-authored seam at 0.5L is the
    # downstream seam the lane must be carried across with matched links (#277).
    stack.push(network, rm.edit.form_lane(
        network, road_id, -1, length * 0.25, -2, rm.LaneType.DRIVING))

    def lane_of(section, odr):
        for lane_id in network.lane_section(section).lanes:
            if network.lane(lane_id).odr_id == odr:
                return lane_id
        return None

    sections = list(network.road(road_id).sections)
    down_index = next((i for i, sec in enumerate(sections)
                       if abs(network.lane_section(sec).s0 - seam_s) < 1e-6), None)
    assert down_index and down_index > 0, "the 0.5L seam was not found"
    up_lane = lane_of(sections[down_index - 1], -2)
    down_lane = lane_of(sections[down_index], -2)
    assert up_lane is not None and down_lane is not None, "formed lane not across the 0.5L seam"
    assert network.lane(down_lane).predecessor is not None, \
        "downstream lane not linked across the seam"
    return "formed lane -2 carried across the 0.5L seam with a matched predecessor link"


# --------------------------------------------------------------------------- #


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("gw2_replay_out")
    out_dir.mkdir(parents=True, exist_ok=True)

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    official: list[tuple[str, bool, str]] = []
    supp: list[tuple[str, bool, str]] = []

    def run_official(title, fn):
        try:
            evidence = fn()
            official.append((title, True, evidence))
            return evidence
        except Exception as exc:  # noqa: BLE001 — report, don't abort the run
            official.append((title, False, f"{type(exc).__name__}: {exc}"))
            return None

    def run_supp(title, fn):
        try:
            evidence = fn()
            supp.append((title, True, evidence))
        except Exception as exc:  # noqa: BLE001
            supp.append((title, False, f"{type(exc).__name__}: {exc}"))

    # Official, command-driven steps accumulate on one network + undo stack.
    result = run_official("2. Create road (3+ waypoints, default profile)",
                          lambda: step2_create_road(network, stack)[1])
    run_official("3. Crossing junction (cross_roads)",
                 lambda: step3_cross(network, stack)[1])
    run_official("4. Extend curved road at BOTH endpoints",
                 lambda: step4_extend_both(network, stack))
    run_official("5. Enclosed-area ground surface", step5_surface)
    run_official("11. Apply road style (replace + preserve)",
                 lambda: step11_road_style(network, stack))

    # Step 12 carves the road authored for the extend step (a free-ended
    # approach with a single final lane section — where junction regeneration
    # would absorb the taper).
    carve_road = next((r for r in network.road_ids
                       if network.road(r).name == "GW-2 extend"), None)
    if carve_road is not None:
        run_official("12. Lane carve (taper to terminus)",
                     lambda: step12_carve(network, stack, carve_road))
    else:
        official.append(("12. Lane carve (taper to terminus)", False, "extend road missing"))

    # SUPP tools (excluded from the official tally).
    run_supp("Lane (type + direction)", lambda: supp_lane(network, stack))
    run_supp("Lane Width (profile taper)", lambda: supp_lane_width(network, stack))
    run_supp("Lane Add (add_lane_span)", lambda: supp_lane_add(network, stack))
    run_supp("Lane Form (across a seam)", lambda: supp_lane_form(network, stack))

    # Cross-cutting: undo x10 / redo x10 byte-identical (mirror gw1_replay).
    undo_evidence = ""
    undo_ok = True
    try:
        assert stack.size >= 10, f"only {stack.size} commands recorded"
        before = rm.write_xodr(network, "gw2_replay")
        for _ in range(10):
            stack.undo(network)
        mid = rm.write_xodr(network, "gw2_replay")
        assert mid != before, "undo x10 changed nothing"
        for _ in range(10):
            stack.redo(network)
        after = rm.write_xodr(network, "gw2_replay")
        assert after == before, "redo x10 did not restore byte-identically"
        undo_evidence = f"{stack.size} commands; byte-identical after undo x10 / redo x10"
    except Exception as exc:  # noqa: BLE001
        undo_ok = False
        undo_evidence = f"{type(exc).__name__}: {exc}"
    official.append(("cross-cutting. undo x10 / redo x10 identical", undo_ok, undo_evidence))

    # Step 23 slice runs last, on the fully-redone network.
    findings: list = []
    try:
        evidence, findings = step23_persist(network, out_dir)
        official.append(("23. Save / reload / validate", True, evidence))
    except Exception as exc:  # noqa: BLE001
        official.append(("23. Save / reload / validate", False, f"{type(exc).__name__}: {exc}"))

    # ----- report -----
    print(f"GW-2 replay — roadmaker {rm.version()} — output in {out_dir}/")
    print("Official steps:")
    for title, ok, evidence in official:
        print(f"  {'PASS' if ok else 'FAIL'}  {title}: {evidence}")
    print("Supplementary P2 tools (not counted in the official tally):")
    for title, ok, evidence in supp:
        print(f"  {'SUPP-PASS' if ok else 'SUPP-WARN'}  {title}: {evidence}")
    if findings:
        warnings = [f for f in findings if f.severity == rm.Severity.WARNING]
        print(f"  validation at end: 0 errors, {len(warnings)} warnings")

    failed = [title for title, ok, _ in official if not ok]
    if failed:
        print(f"GW-2 automatable replay: FAIL ({len(failed)} official step(s) failed)")
        return 1
    print("GW-2 automatable replay: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
