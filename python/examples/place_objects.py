#!/usr/bin/env python3
"""Place road objects (OpenDRIVE <object>, spec chapter 13) on an authored road.

Authors the GS-1 object set on a straight street — a painted crosswalk
(outline object), a signal-carrier pole, and a tree line via <repeat> — then
validates and writes OpenDRIVE.

Usage:
    python place_objects.py output.xodr
"""

import sys

import roadmaker as rm


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Object Demo Street",
    )

    # A crosswalk is an outline object: a closed, paint-filled rectangle in
    # road (s/t) coordinates (lane ids: negative = right of the reference
    # line, positive = left).
    crosswalk = rm.Object()
    crosswalk.odr_id = "1"
    crosswalk.type = rm.ObjectType.CROSSWALK
    crosswalk.s, crosswalk.t = 60.0, 0.0
    crosswalk.length, crosswalk.width = 8.0, 4.0
    outline = rm.ObjectOutline()
    outline.closed = True
    outline.fill_type = "paint"
    outline.corners = [
        rm.OutlineCorner(58.0, -3.5),
        rm.OutlineCorner(62.0, -3.5),
        rm.OutlineCorner(62.0, 3.5),
        rm.OutlineCorner(58.0, 3.5),
    ]
    crosswalk.outlines = [outline]
    network.add_object(road_id, crosswalk)

    # A pole (future signal carrier) just before the crosswalk, right side.
    pole = rm.Object()
    pole.odr_id = "2"
    pole.type = rm.ObjectType.POLE
    pole.s, pole.t = 55.0, -6.0
    pole.radius, pole.height = 0.06, 4.0
    network.add_object(road_id, pole)

    # A tree line: one template tree repeated every 15 m along the road.
    tree = rm.Object()
    tree.odr_id = "3"
    tree.type = rm.ObjectType.TREE
    tree.s, tree.t = 5.0, 8.0
    tree.radius, tree.height = 0.5, 6.0
    repeat = rm.ObjectRepeat()
    repeat.s, repeat.length, repeat.distance = 5.0, 110.0, 15.0
    repeat.t_start = repeat.t_end = 8.0
    tree.repeats = [repeat]
    network.add_object(road_id, tree)

    print(f"placed {network.object_count} objects on {network.road(road_id)!r}")
    for object_id in network.objects_of(road_id):
        print(f"  {network.object(object_id)!r}")

    findings = rm.validate_network(network)
    object_findings = [f for f in findings if "object" in f.rule_id]
    print(f"validator: {len(object_findings)} object findings")

    rm.save_xodr(network, xodr_path, name="place_objects demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
