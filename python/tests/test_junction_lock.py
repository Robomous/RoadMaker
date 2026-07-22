# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Junction lock and span membership through the bindings (#319, p4-s4).

`Junction.locked` keeps a hand-tuned junction out of the AUTOMATIC
regeneration loops; `edit.set_junction_locked` toggles it, and unlocking a
junction whose arms no longer plan removes it together with its connecting
roads. `Junction.spans` exposes the virtual (span) junction membership, and
`edit.create_span_junction` authors one: a §12.7 virtual junction over an
UNINTERRUPTED road (a mid-road crosswalk, or the same crossing over two
parallel carriageways).
"""

import pathlib
import subprocess
import sys

import pytest

import roadmaker as rm


@pytest.fixture
def crossing():
    """(network, stack, junction_id) for a four-arm crossing."""
    net = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    pose = rm.edit.assembly.Pose(0.0, 0.0, 0.0)
    stack.push(net, rm.edit.assembly.x_intersection(net, pose))
    return net, stack, net.junction_ids[0]


def test_a_fresh_junction_is_unlocked_and_has_no_spans(crossing):
    net, _stack, jid = crossing
    junction = net.junction(jid)
    assert junction.locked is False
    assert list(junction.spans) == []


def test_lock_and_unlock_round_trip_through_the_stack(crossing):
    net, stack, jid = crossing
    connections = len(net.junction(jid).connections)

    stack.push(net, rm.edit.set_junction_locked(net, jid, True))
    assert net.junction(jid).locked is True
    # Locking is a pure value edit: the turn set is untouched.
    assert len(net.junction(jid).connections) == connections

    stack.undo(net)
    assert net.junction(jid).locked is False

    stack.redo(net)
    assert net.junction(jid).locked is True

    stack.push(net, rm.edit.set_junction_locked(net, jid, False))
    assert net.junction(jid).locked is False


def test_a_no_op_lock_is_refused(crossing):
    net, stack, jid = crossing
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_junction_locked(net, jid, False))

    stack.push(net, rm.edit.set_junction_locked(net, jid, True))
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_junction_locked(net, jid, True))


def test_a_foreign_junction_has_no_regeneration_to_lock(crossing):
    net, stack, _jid = crossing
    # No arms and no spans: what a junction read from someone else's file
    # looks like.
    foreign = net.create_junction("99", "foreign")
    assert list(net.junction(foreign).arms) == []
    assert list(net.junction(foreign).spans) == []
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_junction_locked(net, foreign, True))


def test_span_arm_type_is_exposed():
    assert rm.SpanArm.__name__ == "SpanArm"
    for field in ("road", "s_start", "s_end"):
        assert hasattr(rm.SpanArm, field)

def test_removing_and_re_adding_an_arm_round_trips(crossing):
    net, stack, jid = crossing
    stack.push(net, rm.edit.set_junction_locked(net, jid, True))
    arms = list(net.junction(jid).arms)
    assert len(arms) == 4
    departing = arms[3]

    stack.push(net, rm.edit.remove_junction_arm(net, jid, departing))
    assert len(net.junction(jid).arms) == 3

    stack.undo(net)
    assert len(net.junction(jid).arms) == 4

    stack.redo(net)
    assert len(net.junction(jid).arms) == 3

    # The freed end can rejoin the junction it just left.
    stack.push(net, rm.edit.add_junction_arm(net, jid, departing))
    assert len(net.junction(jid).arms) == 4


def test_membership_edits_require_the_lock(crossing):
    net, stack, jid = crossing
    arm = list(net.junction(jid).arms)[0]
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.remove_junction_arm(net, jid, arm))
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.add_junction_arm(net, jid, arm))


def test_merging_a_junction_with_itself_is_refused(crossing):
    net, stack, jid = crossing
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.merge_junctions(net, jid, jid))


def _through_road(net, stack, y):
    """A straight 120 m two-lane road at `y` — the substrate a span covers."""
    stack.push(
        net,
        rm.edit.create_road(
            [(0.0, y), (120.0, y)], rm.LaneProfile.two_lane_default(), ""
        ),
    )
    return net.road_ids[-1]


def test_create_span_junction_single_road(crossing):
    net, stack, _jid = crossing
    road = _through_road(net, stack, -200.0)
    junctions = len(net.junction_ids)

    stack.push(net, rm.edit.create_span_junction(net, [rm.SpanArm(road, 50.0, 56.5)]))
    assert len(net.junction_ids) == junctions + 1

    span_junction = net.junction(net.junction_ids[-1])
    # ASAM OpenDRIVE 1.9.0 §12.7: the main road is uninterrupted, so a virtual
    # junction has no arms, no connections and no road links — and it is always
    # locked, because there is no derivation behind it.
    assert span_junction.locked is True
    assert list(span_junction.arms) == []
    assert list(span_junction.connections) == []
    assert net.road(road).predecessor is None
    assert net.road(road).successor is None
    spans = list(span_junction.spans)
    assert len(spans) == 1
    assert spans[0].road == road
    assert spans[0].s_start == pytest.approx(50.0)
    assert spans[0].s_end == pytest.approx(56.5)

    stack.undo(net)
    assert len(net.junction_ids) == junctions
    stack.redo(net)
    assert len(net.junction_ids) == junctions + 1


def test_create_span_junction_two_parallel_roads(crossing):
    net, stack, _jid = crossing
    north = _through_road(net, stack, -200.0)
    south = _through_road(net, stack, -212.0)

    stack.push(
        net,
        rm.edit.create_span_junction(
            net, [rm.SpanArm(north, 40.0, 44.0), rm.SpanArm(south, 41.5, 45.5)]
        ),
    )
    spans = list(net.junction(net.junction_ids[-1]).spans)
    assert [span.road for span in spans] == [north, south]
    assert spans[1].s_end == pytest.approx(45.5)


def test_a_created_span_junction_is_structurally_locked(crossing):
    net, stack, _jid = crossing
    road = _through_road(net, stack, -200.0)
    stack.push(net, rm.edit.create_span_junction(net, [rm.SpanArm(road, 50.0, 56.5)]))
    with pytest.raises(ValueError):
        stack.push(net, rm.edit.set_junction_locked(net, net.junction_ids[-1], False))


@pytest.mark.parametrize(
    "spans",
    [
        pytest.param([], id="no_spans"),
        pytest.param([(0, 60.0, 40.0)], id="inverted"),
        pytest.param([(0, 40.0, 40.0)], id="zero_length"),
        pytest.param([(0, -5.0, 10.0)], id="before_the_start"),
        pytest.param([(0, 100.0, 500.0)], id="past_the_end"),
        pytest.param([(0, 10.0, 14.0), (0, 20.0, 24.0)], id="same_road_twice"),
        pytest.param(
            [(0, 10.0, 14.0), (1, 10.0, 14.0), (2, 10.0, 14.0)], id="three_spans"
        ),
    ],
)
def test_bad_spans_are_refused(crossing, spans):
    net, stack, _jid = crossing
    roads = [_through_road(net, stack, y) for y in (-200.0, -212.0, -224.0)]
    with pytest.raises(ValueError):
        stack.push(
            net,
            rm.edit.create_span_junction(
                net, [rm.SpanArm(roads[i], a, b) for i, a, b in spans]
            ),
        )


def test_a_connecting_road_cannot_carry_a_span(crossing):
    net, stack, jid = crossing
    connecting = net.junction(jid).connections[0].connecting_road
    with pytest.raises(ValueError):
        stack.push(
            net, rm.edit.create_span_junction(net, [rm.SpanArm(connecting, 0.0, 1.0)])
        )


def test_the_example_script_runs_clean(tmp_path):
    """python/examples/junction_control.py is the sprint's hand script."""
    example = (
        pathlib.Path(__file__).resolve().parents[1] / "examples" / "junction_control.py"
    )
    result = subprocess.run(
        [sys.executable, str(example), str(tmp_path / "junction_control.xodr")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "validate_network: 0 findings" in result.stdout
