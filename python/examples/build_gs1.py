"""Build GS-1 — the M3a "Urban intersection" golden scene.

Dogfoods the kernel edit layer end to end to author
``assets/samples/gs1_urban_intersection.xodr`` — the acceptance artifact for
M3a (docs/roadmap/golden_scenes/gs1_urban_intersection.md): a signalized 4-arm
urban junction with textured surfaces, crosswalks / stop lines / lane arrows on
every arm, four traffic lights, two static signs, and a line of street trees.

Everything here is a real ``edit::Command`` on an ``EditStack`` — the same path
the editor uses — so the scene round-trips and validates like any authored file.

Run:  python build_gs1.py [out.xodr]
      (defaults to assets/samples/gs1_urban_intersection.xodr)
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import roadmaker as rm

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO_ROOT / "assets" / "samples" / "gs1_urban_intersection.xodr"

TREE_MODELS = ["tree_pine", "tree_oak", "tree_birch", "tree_poplar"]


def arm_roads(network: rm.RoadNetwork) -> list:
    """The four stub arms — every road that is not a junction connecting road."""
    arms = []
    for road_id in network.road_ids:
        road = network.road(road_id)
        if not road.junction.valid:
            arms.append(road_id)
    return arms


def junction_facing_end(network: rm.RoadNetwork, road_id) -> tuple[float, float]:
    """(s of the junction-facing end, sign) for a stub arm: the endpoint nearer
    the junction centre (origin) is where the signals/marks cluster. `sign` is
    +1 when that end is at s = length (place upstream at s - d), -1 at s = 0."""
    road = network.road(road_id)
    length = road.plan_view.length
    start = road.plan_view.evaluate(0.0)
    end = road.plan_view.evaluate(length)
    d_start = math.hypot(start.x, start.y)
    d_end = math.hypot(end.x, end.y)
    if d_end < d_start:
        return length, +1.0
    return 0.0, -1.0


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # 1. The 4-arm junction: an urban profile (driving lanes + sidewalks behind a
    #    curb) with 40 m arms, generated as one undoable command.
    params = rm.edit.assembly.IntersectionParams()
    params.arm_length_m = 40.0
    params.profile = rm.LaneProfile.urban_sidewalk()
    stack.push(network, rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params))
    junction = network.find_junction("1")

    # 2. Junction marks on every arm — crosswalks, stop lines, lane arrows.
    for road, obj in rm.edit.junction_crosswalks(network, junction):
        stack.push(network, rm.edit.add_object(network, road, obj))
    for road, obj in rm.edit.junction_stop_lines(network, junction):
        stack.push(network, rm.edit.add_object(network, road, obj))
    for road, obj in rm.edit.junction_lane_arrows(network, junction):
        stack.push(network, rm.edit.add_object(network, road, obj))

    # 3. Signals: one traffic light on every arm, plus two static signs.
    arms = arm_roads(network)
    for index, road_id in enumerate(arms):
        s_end, sign = junction_facing_end(network, road_id)
        s_signal = s_end - (sign * 8.0)  # 8 m upstream of the junction-facing end
        light = rm.Signal()
        light.odr_id = f"tl{index + 1}"
        light.type = "1000001"  # OpenDRIVE traffic-light catalog type
        light.subtype = "-1"
        light.country = "OpenDRIVE"
        light.dynamic = True
        light.s = s_signal
        light.t = -6.0  # right of the reference line
        stack.push(network, rm.edit.add_signal(network, road_id, light))

    # A speed-limit sign and a pedestrian-crossing warning sign on the first two arms.
    static_signs = [("274", "50"), ("133", "10")]  # DE speed-limit 50; pedestrian crossing
    for index, (sign_type, sub) in enumerate(static_signs):
        road_id = arms[index]
        s_end, sign = junction_facing_end(network, road_id)
        signal = rm.Signal()
        signal.odr_id = f"sg{index + 1}"
        signal.type = sign_type
        signal.subtype = sub
        signal.country = "DE"
        signal.dynamic = False
        signal.s = s_end - (sign * 18.0)
        signal.t = -6.5
        stack.push(network, rm.edit.add_signal(network, road_id, signal))

    # 4. Street trees along the arms — a couple per side, behind the sidewalk.
    tree_index = 0
    for road_id in arms:
        length = network.road(road_id).plan_view.length
        for s in (length * 0.35, length * 0.65):
            for t in (-8.5, 8.5):
                tree = rm.Object()
                tree.odr_id = f"tree{tree_index + 1}"
                tree.name = TREE_MODELS[tree_index % len(TREE_MODELS)]
                tree.type = rm.ObjectType.TREE
                tree.s = s
                tree.t = t
                tree.radius = 1.2
                tree.height = 4.5
                stack.push(network, rm.edit.add_object(network, road_id, tree))
                tree_index += 1

    diagnostics = rm.validate_network(network)
    errors = [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    print(
        f"GS-1: roads={network.road_count} junctions={network.junction_count} "
        f"objects={network.object_count} signals={network.signal_count} "
        f"diagnostics={len(diagnostics)} errors={len(errors)}"
    )
    assert not errors, f"GS-1 must validate with zero errors: {errors}"

    out_path.parent.mkdir(parents=True, exist_ok=True)
    rm.save_xodr(network, str(out_path), "GS-1 Urban intersection")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
