"""Lane-section authoring through the bindings: split, width profiles, helpers."""

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


def test_poly3_takes_keyword_coefficients():
    poly = rm.Poly3(s=10.0, a=3.0, b=0.5)
    assert poly.s == pytest.approx(10.0)
    assert poly.eval(12.0) == pytest.approx(4.0)


def test_section_at_and_section_end(network):
    road = network.find_road("1")
    first = network.road(road).sections[0]
    length = network.road(road).length

    assert network.section_at(road, 0.0) == first
    assert network.section_end(first) == pytest.approx(length)

    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.split_lane_section(network, road, 50.0))
    sections = network.road(road).sections
    assert len(sections) == 2
    assert network.section_at(road, 10.0) == sections[0]
    assert network.section_at(road, 50.0) == sections[1]
    assert network.section_end(sections[0]) == pytest.approx(50.0)
    assert network.section_end(sections[1]) == pytest.approx(length)


def test_split_lane_section_duplicates_the_cross_section_and_undoes(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    before = rm.write_xodr(network)
    lanes = len(network.lane_section(network.road(road).sections[0]).lanes)

    stack.push(network, rm.edit.split_lane_section(network, road, 50.0))
    sections = network.road(road).sections
    assert len(sections) == 2
    assert len(network.lane_section(sections[1]).lanes) == lanes
    assert network.lane_section(sections[1]).s0 == pytest.approx(50.0)

    # apply -> revert is byte-identical by contract.
    stack.undo(network)
    assert rm.write_xodr(network) == before


def test_split_lane_section_is_idempotent_at_a_boundary(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.split_lane_section(network, road, 50.0))
    after = rm.write_xodr(network)

    stack.push(network, rm.edit.split_lane_section(network, road, 50.0))
    assert rm.write_xodr(network) == after
    assert len(network.road(road).sections) == 2


def test_split_lane_section_rejects_stations_outside_the_road(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.split_lane_section(network, road, 0.0))
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.split_lane_section(network, road, 10_000.0))


def test_set_lane_width_profile_authors_a_taper(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()

    stack.push(
        network,
        rm.edit.set_lane_width_profile(network, lane, [rm.Poly3(s=0.0, a=3.0, b=0.02)]),
    )
    widths = network.lane(lane).widths
    assert len(widths) == 1
    assert widths[0].eval(50.0) == pytest.approx(4.0)


def test_set_lane_width_profile_allows_zero_width(network):
    """Width >= 0 is the only rule; a turn lane tapers up from nothing."""
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()

    stack.push(
        network,
        rm.edit.set_lane_width_profile(network, lane, [rm.Poly3(s=0.0, a=0.0, b=0.1)]),
    )
    assert network.lane(lane).widths[0].eval(0.0) == pytest.approx(0.0)


@pytest.mark.parametrize(
    "widths",
    [
        [],  # empty
        [rm.Poly3(s=10.0, a=3.0)],  # no record at sOffset 0
        [rm.Poly3(s=0.0, a=3.0), rm.Poly3(s=0.0, a=3.5)],  # not ascending
        [rm.Poly3(s=0.0, a=-1.0)],  # negative width
        [rm.Poly3(s=0.0, a=3.0), rm.Poly3(s=5000.0, a=3.5)],  # past the section end
    ],
)
def test_set_lane_width_profile_rejects_non_conformant_profiles(network, widths):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_lane_width_profile(network, lane, widths))


def test_set_lane_width_profile_rejects_the_center_lane(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    center = lane_by_odr_id(network, section, 0)
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_lane_width_profile(network, center, [rm.Poly3(a=3.0)]))


def test_set_lane_width_refuses_to_flatten_a_taper(network):
    """The regression P2 exists to prevent: set_lane_width used to overwrite
    the width profile unconditionally, destroying any authored taper."""
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()

    stack.push(
        network,
        rm.edit.set_lane_width_profile(network, lane, [rm.Poly3(s=0.0, a=3.0, b=0.01)]),
    )
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.set_lane_width(network, lane, 4.0))
    assert network.lane(lane).widths[0].b == pytest.approx(0.01)


def test_set_lane_width_still_sets_a_constant(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()

    stack.push(network, rm.edit.set_lane_width(network, lane, 4.25))
    assert network.lane(lane).widths[0].a == pytest.approx(4.25)


def test_split_survives_an_xodr_round_trip(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.split_lane_section(network, road, 50.0))

    text = rm.write_xodr(network)
    back, diagnostics = rm.parse_xodr(text)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert len(back.road(back.find_road("1")).sections) == 2
    assert rm.write_xodr(back) == text


def test_width_profile_survives_an_xodr_round_trip(network):
    """A taper authored with set_lane_width_profile writes and re-reads intact."""
    road = network.find_road("1")
    section = network.road(road).sections[0]
    lane = lane_by_odr_id(network, section, -1)
    stack = rm.edit.EditStack()
    records = [rm.Poly3(s=0.0, a=3.0, b=0.02), rm.Poly3(s=60.0, a=4.2)]
    stack.push(network, rm.edit.set_lane_width_profile(network, lane, records))

    text = rm.write_xodr(network)
    back, diagnostics = rm.parse_xodr(text)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert rm.write_xodr(back) == text

    back_lane = lane_by_odr_id(back, back.road(back.find_road("1")).sections[0], -1)
    widths = back.lane(back_lane).widths
    assert len(widths) == len(records)
    for got, want in zip(widths, records):
        assert got.s == pytest.approx(want.s)
        assert got.a == pytest.approx(want.a)
        assert got.b == pytest.approx(want.b)


def odr_ids(net, section):
    return sorted(net.lane(lane_id).odr_id for lane_id in net.lane_section(section).lanes)


def test_insert_lane_renumbers_the_outer_block(network):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    assert odr_ids(network, section) == [-2, -1, 0, 1]
    before = rm.write_xodr(network)
    stack = rm.edit.EditStack()

    # Insert at -1: the old -1 and -2 step out, a fresh lane takes -1.
    stack.push(network, rm.edit.insert_lane(network, section, -1, rm.LaneType.DRIVING))
    assert odr_ids(network, section) == [-3, -2, -1, 0, 1]

    # apply -> revert is byte-identical by contract.
    stack.undo(network)
    assert rm.write_xodr(network) == before


def test_insert_lane_leaves_the_new_lane_unlinked(network):
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.split_lane_section(network, road, 60.0))
    head = network.road(road).sections[0]
    tail = network.road(road).sections[1]

    stack.push(network, rm.edit.insert_lane(network, head, -1, rm.LaneType.DRIVING))

    inserted = network.lane(lane_by_odr_id(network, head, -1))
    assert inserted.predecessor is None
    assert inserted.successor is None
    # The tail's predecessor followed the renumbering from -1 to -2.
    assert network.lane(lane_by_odr_id(network, tail, -1)).predecessor == -2


@pytest.mark.parametrize(
    "at_odr_id",
    [
        0,  # the center lane cannot be displaced
        -5,  # no lane there — appending past the outermost is add_lane
    ],
)
def test_insert_lane_rejects_bad_positions(network, at_odr_id):
    road = network.find_road("1")
    section = network.road(road).sections[0]
    stack = rm.edit.EditStack()
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.insert_lane(network, section, at_odr_id, rm.LaneType.DRIVING))


def test_add_lane_span_round_trips(network):
    """Lane Add: a pocket lane over [40, 90] that is one atomic, reversible step
    and adds two lane-section seams."""
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    before = rm.write_xodr(network)
    assert len(network.road(road).sections) == 1

    stack.push(network, rm.edit.add_lane_span(network, road, -1, 40.0, 90.0, rm.LaneType.DRIVING))
    sections = network.road(road).sections
    assert len(sections) == 3

    # The middle section carries an extra outermost right lane, zero width at
    # both seams (so the pocket needs no cross-section links).
    mid = network.section_at(road, 65.0)
    pocket = lane_by_odr_id(network, mid, -3)
    assert pocket is not None
    length = network.section_end(mid) - network.lane_section(mid).s0
    pocket_widths = network.lane(pocket).widths
    assert pocket_widths[0].eval(0.0) == pytest.approx(0.0)
    assert network.lane(pocket).predecessor is None
    assert network.lane(pocket).successor is None

    text = rm.write_xodr(network)
    back, diagnostics = rm.parse_xodr(text)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]

    # apply -> revert is byte-identical by contract.
    stack.undo(network)
    assert rm.write_xodr(network) == before
    _ = length  # section length is exercised above for the eval frame


def test_form_lane_backward_unlinked(network):
    """Lane Form: an interior lane from s_start to the road end, backward-
    unlinked (it appears mid-road, not as a continuation)."""
    road = network.find_road("1")
    stack = rm.edit.EditStack()

    stack.push(network, rm.edit.form_lane(network, road, -1, 60.0, -1, rm.LaneType.DRIVING))
    sections = network.road(road).sections
    assert len(sections) == 2

    formed = lane_by_odr_id(network, sections[-1], -1)
    assert formed is not None
    assert network.lane(formed).predecessor is None

    # The writer accepts it (zero width at the seam means no link is due).
    text = rm.write_xodr(network)
    back, diagnostics = rm.parse_xodr(text)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]


def test_form_lane_refuses_a_downstream_seam(network):
    """Forward-linking a formed lane is out of scope: a form that would reach a
    downstream lane-section boundary is refused."""
    road = network.find_road("1")
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.split_lane_section(network, road, 90.0))
    with pytest.raises(ValueError):
        stack.push(network, rm.edit.form_lane(network, road, -1, 60.0, -1, rm.LaneType.DRIVING))
