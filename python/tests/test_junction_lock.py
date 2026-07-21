"""Junction lock and span membership through the bindings (#319, p4-s4).

`Junction.locked` keeps a hand-tuned junction out of the AUTOMATIC
regeneration loops; `edit.set_junction_locked` toggles it, and unlocking a
junction whose arms no longer plan removes it together with its connecting
roads. `Junction.spans` exposes the virtual (span) junction membership.
"""

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
