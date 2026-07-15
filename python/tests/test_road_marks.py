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


@pytest.fixture
def junction_network():
    """A 4-arm urban intersection — the GS-1 construction."""
    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    params = rm.edit.assembly.IntersectionParams()
    params.arm_length_m = 40.0
    params.profile = rm.LaneProfile.urban_sidewalk()
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    return network, stack, network.find_junction("1")


def test_junction_center_marks_author_dual_yellow_on_every_arm(junction_network):
    network, stack, junction = junction_network
    marks = rm.edit.junction_center_marks(network, junction)
    assert len(marks) == 4  # one lane 0 per arm

    for lane_id, mark in marks:
        assert network.lane(lane_id).odr_id == 0
        assert mark.type == rm.RoadMarkType.SOLID_SOLID
        assert mark.color == rm.RoadMarkColor.YELLOW
        # Bare mark: the mesh synthesizes the two stripes from `width`.
        assert mark.lines == []
        stack.push(network, rm.edit.set_road_mark(network, lane_id, mark))

    xml = rm.write_xodr(network, "center-marks")
    assert 'type="solid solid"' in xml
    assert 'color="yellow"' in xml
    assert rm.validate_network(network) == []


def test_junction_center_marks_params_override_type_and_color(junction_network):
    network, _stack, junction = junction_network
    marks = rm.edit.junction_center_marks(
        network, junction, type=rm.RoadMarkType.SOLID, color=rm.RoadMarkColor.WHITE, width=0.2
    )
    assert marks
    for _lane_id, mark in marks:
        assert mark.type == rm.RoadMarkType.SOLID
        assert mark.color == rm.RoadMarkColor.WHITE
        assert mark.width == pytest.approx(0.2)


def test_junction_lane_arrows_glyph_chooser_selects_per_lane(junction_network):
    network, _stack, junction = junction_network
    first = network.find_road("1")
    seen = []

    def glyph(road, lane_odr_id):
        seen.append(lane_odr_id)
        # "" declines and takes the arrowStraight default.
        return "arrowLeft" if road == first else ""

    arrows = rm.edit.junction_lane_arrows(network, junction, glyph)
    assert len(seen) == len(arrows)  # called once per approach lane
    subtypes = sorted(obj.subtype for _road, obj in arrows)
    assert subtypes == ["arrowLeft", "arrowStraight", "arrowStraight", "arrowStraight"]


def test_junction_lane_arrows_default_to_straight(junction_network):
    network, _stack, junction = junction_network
    arrows = rm.edit.junction_lane_arrows(network, junction)
    assert {obj.subtype for _road, obj in arrows} == {"arrowStraight"}
