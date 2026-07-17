"""Road styles through the bindings (edit.apply_road_style, p2-s8)."""

import pytest

import roadmaker as rm


@pytest.fixture
def network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (60.0, 0.0), (120.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "Straight",
        "1",
    )
    return net


def lane_by_odr_id(net, section_id, odr_id):
    for lane_id in net.lane_section(section_id).lanes:
        if net.lane(lane_id).odr_id == odr_id:
            return lane_id
    return None


def test_urban_two_lane_preset_contents():
    style = rm.RoadStyle.urban_two_lane()
    assert len(style.left) == 2
    assert len(style.right) == 2
    # Inner same-direction lane: dashed white divider on its outer boundary.
    assert style.left[0].type == rm.LaneType.DRIVING
    assert style.left[0].width.a == pytest.approx(3.5)
    assert style.left[0].outer_mark is not None
    assert style.left[0].outer_mark.type == rm.RoadMarkType.BROKEN
    assert style.left[0].outer_mark.color == rm.RoadMarkColor.WHITE
    assert style.center_mark.type == rm.RoadMarkType.SOLID
    assert style.center_mark.color == rm.RoadMarkColor.YELLOW


def test_apply_road_style_flattens_and_replaces(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road, rm.RoadStyle.urban_two_lane()))

    sections = network.road(road).sections
    assert len(sections) == 1  # flattened to one section
    section = network.lane_section(sections[0])
    assert len(section.lanes) == 5  # center + 2 each side
    inner = network.lane(lane_by_odr_id(network, sections[0], 1))
    assert inner.road_marks[0].type == rm.RoadMarkType.BROKEN
    assert inner.road_marks[0].color == rm.RoadMarkColor.WHITE


def test_apply_road_style_round_trips_and_ids_are_stable(network):
    road = network.find_road("1")
    original_sections = list(network.road(road).sections)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road, rm.RoadStyle.highway()))
    assert len(network.road(road).sections) == 1

    stack.undo(network)
    # The road id survives and the original sections are restored in place.
    assert network.road(road) is not None
    assert list(network.road(road).sections) == original_sections
    stack.redo(network)
    assert len(network.road(road).sections) == 1


def test_apply_road_style_preserves_name_and_geometry(network):
    road = network.find_road("1")
    length = network.road(road).length
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.apply_road_style(network, road, rm.RoadStyle.urban_two_lane()))
    assert network.road(road).name == "Straight"
    assert network.road(road).length == pytest.approx(length)


def test_apply_road_style_rejects_a_style_with_no_lanes(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.apply_road_style(network, road, rm.RoadStyle()))
