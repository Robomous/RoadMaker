#!/usr/bin/env python3
"""Parametric crosswalk assets (p3-s2): author, edit, and export.

Builds a T-junction, drops one parametric crosswalk per arm via
edit.junction_crosswalks (materializing an asset's stripe geometry + material
into each object's outline, <markings>, and rm:crosswalk userData), then edits
the asset once — re-materializing every instance in a single undoable
edit.update_objects command — and writes OpenDRIVE.

Usage:
    python crosswalk_assets.py output.xodr
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    arms = [
        rm.author_clothoid_road(network, [(-40.0, 0.0), (-6.0, 0.0)], rm.LaneProfile.two_lane_default(), name="w"),
        rm.author_clothoid_road(network, [(40.0, 0.0), (6.0, 0.0)], rm.LaneProfile.two_lane_default(), name="e"),
        rm.author_clothoid_road(network, [(0.0, -40.0), (0.0, -6.0)], rm.LaneProfile.two_lane_default(), name="s"),
    ]
    ends = [rm.RoadEnd(a, rm.ContactPoint.END) for a in arms]
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.junction_ids[0]

    # The parametric asset: dashed white zebra, 0.4 m stripes / 0.6 m gaps.
    params = rm.edit.CrosswalkParams()
    params.dash_length_m = 0.4
    params.dash_gap_m = 0.6
    params.material = "material.paint_white"
    params.asset = "crosswalk.zebra"
    params.category = "crosswalk"

    instances = rm.edit.junction_crosswalks(network, junction, params)
    object_ids = []
    for road, crosswalk in instances:
        command = rm.edit.add_object(network, road, crosswalk)
        stack.push(network, command)
        object_ids.append(network.objects_of(road)[-1])
    print(f"placed {len(object_ids)} crosswalk instances")

    # Edit the asset once: widen the stripes to solid on every following
    # instance, in ONE undoable command (the editor's asset-edit path).
    updates = []
    for object_id in object_ids:
        updated = rm.Object(network.object(object_id))
        data = updated.crosswalk
        data.dash_length = 0.0  # solid
        updated.crosswalk = data
        updates.append((object_id, updated))
    stack.push(network, rm.edit.update_objects(network, updates, "Edit Crosswalk Asset"))
    print("re-materialized all instances as solid crossings (one undo entry)")

    stack.undo(network)  # asset edit is undoable as a unit
    assert network.object(object_ids[0]).crosswalk.dash_length == 0.4
    stack.redo(network)
    assert network.object(object_ids[0]).crosswalk.dash_length == 0.0

    findings = [f for f in rm.validate_network(network) if f.severity == rm.Severity.ERROR]
    if findings:
        for finding in findings:
            print(f"  validation error: {finding.message}", file=sys.stderr)
        return 1

    rm.save_xodr(network, xodr_path, name="Crosswalk Asset Demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
