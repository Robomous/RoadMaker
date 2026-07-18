#!/usr/bin/env python3
"""Assign lane surface materials (OpenDRIVE §11.8.2) and round-trip them.

Builds a straight street and gives lane -1 a two-record <material> profile —
new asphalt over its first half, worn asphalt over the second — then saves,
reloads, and asserts the records survive byte-for-byte.

A material record is:

    LaneMaterial(s_offset, friction, roughness, surface)   (s_offset SECTION-local)

`surface` is the OpenDRIVE "surface material code, depending on application";
RoadMaker writes "rm:<id>" so an id is recognisable as ours in a foreign file.
`set_lane_material` refuses the centre lane (the standard forbids material
there), non-ascending sOffsets, and negative friction/roughness.

Usage:
    python lane_material.py output.xodr
"""

import sys

import roadmaker as rm


def lane_by_odr_id(network, section_id, odr_id):
    """The lane with this OpenDRIVE id in `section_id`, or None."""
    for lane_id in network.lane_section(section_id).lanes:
        if network.lane(lane_id).odr_id == odr_id:
            return lane_id
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="Material Demo",
    )

    stack = rm.edit.EditStack()
    section_id = network.road(road_id).sections[0]
    lane = lane_by_odr_id(network, section_id, -1)

    # New asphalt for [0, 60), worn for [60, end). RoadMaker sets friction from
    # the catalog's nominal value; roughness is optional.
    stack.push(
        network,
        rm.edit.set_lane_material(
            network,
            lane,
            [
                rm.LaneMaterial(s_offset=0.0, friction=0.9, roughness=0.012, surface="rm:asphalt"),
                rm.LaneMaterial(s_offset=60.0, friction=0.7, surface="rm:asphalt_worn"),
            ],
        ),
    )

    materials = network.lane(lane).materials
    print(f"lane -1 material records: {len(materials)}")
    for material in materials:
        print(
            f"  sOffset = {material.s_offset:5.1f} m  friction = {material.friction}"
            f"  surface = {material.surface}"
        )

    # The centre lane may not carry material (center_lane_no_material).
    centre = lane_by_odr_id(network, section_id, 0)
    try:
        stack.push(
            network,
            rm.edit.set_lane_material(network, centre, [rm.LaneMaterial(friction=0.9)]),
        )
        print("BUG: assigned material to the centre lane")
    except ValueError as exc:
        print(f"material on the centre lane refused: {exc}")

    # apply -> revert is byte-identical by contract.
    while stack.can_undo:
        stack.undo(network)
    assert not network.lane(lane).materials, "undo left material records behind"
    while stack.can_redo:
        stack.redo(network)

    rm.save_xodr(network, xodr_path, name="lane_material demo")
    print(f"wrote {xodr_path}")

    # Reload and confirm the assignment survived the round-trip.
    reloaded, _ = rm.load_xodr(xodr_path)
    reloaded_section = reloaded.road(reloaded.find_road(network.road(road_id).odr_id)).sections[0]
    reloaded_lane = lane_by_odr_id(reloaded, reloaded_section, -1)
    reloaded_materials = reloaded.lane(reloaded_lane).materials
    assert len(reloaded_materials) == 2, "material records did not round-trip"
    assert reloaded_materials[0].surface == "rm:asphalt"
    assert reloaded_materials[1].surface == "rm:asphalt_worn"
    print("round-trip OK: both material records reloaded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
