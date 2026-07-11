"""Road-mark completions: color + multi-line geometry bindings (issue #69)."""

import pytest

import roadmaker as rm


@pytest.fixture
def network_with_road():
    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="r",
    )
    return network, road_id


def _center_lane(network, road_id):
    road = network.road(road_id)
    section = network.lane_section(road.sections[0])
    for lane_id in section.lanes:
        lane = network.lane(lane_id)
        if lane.odr_id == 0:
            return lane
    raise AssertionError("no center lane")


def test_road_mark_color_and_lines_round_trip(network_with_road, tmp_path):
    network, road_id = network_with_road
    lane = _center_lane(network, road_id)
    mark = rm.RoadMark(type=rm.RoadMarkType.SOLID_SOLID, width=0.3, color=rm.RoadMarkColor.YELLOW)
    mark.lines = [
        rm.RoadMarkLine(width=0.12, t_offset=0.1),
        rm.RoadMarkLine(width=0.12, t_offset=-0.1),
    ]
    lane.road_marks = [mark]

    path = tmp_path / "marks.xodr"
    rm.save_xodr(network, path, name="marks test")

    reloaded, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    lane2 = _center_lane(reloaded, reloaded.find_road(network.road(road_id).odr_id))
    assert len(lane2.road_marks) == 1
    restored = lane2.road_marks[0]
    assert restored.type == rm.RoadMarkType.SOLID_SOLID
    assert restored.color == rm.RoadMarkColor.YELLOW
    assert len(restored.lines) == 2
    assert restored.lines[0].t_offset == pytest.approx(0.1)
    assert restored.lines[1].t_offset == pytest.approx(-0.1)


def test_simple_mark_keeps_empty_lines(network_with_road):
    network, road_id = network_with_road
    lane = _center_lane(network, road_id)
    lane.road_marks = [rm.RoadMark(type=rm.RoadMarkType.SOLID, width=0.12)]
    assert lane.road_marks[0].color == rm.RoadMarkColor.STANDARD
    assert lane.road_marks[0].lines == []


def test_dual_yellow_meshes_without_error(network_with_road):
    # Mesh internals aren't Python-exposed (mesh determinism is covered in the
    # C++ suite); this just exercises build_network_mesh over a dual mark.
    network, road_id = network_with_road
    lane = _center_lane(network, road_id)
    lane.road_marks = [rm.RoadMark(type=rm.RoadMarkType.SOLID_SOLID, width=0.15)]
    mesh = rm.build_network_mesh(network)
    assert mesh.vertex_count > 0
