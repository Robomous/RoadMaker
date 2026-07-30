# Copyright 2026 Robomous
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Python parity for composite prop assemblies (p6-s9, #323).

Not to be confused with ``rm.edit.assembly``, the T/X road-junction builder that
predates this and is exercised by test_junction_corners.py.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest
import roadmaker as rm


@pytest.fixture(autouse=True)
def _clear_overlay():
    """The overlay is process-wide, so a test that registers must not leak into
    the next one. Same hazard, same guard as test_prop_import.py's."""
    yield
    rm.props.clear_project_assemblies()


def _road(network: rm.RoadNetwork) -> rm.RoadId:
    return rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (200.0, 0.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Assembly Street",
    )


def _grove(assembly_id: str = "my_grove", model: str = "tree_oak") -> rm.props.PropAssembly:
    """A three-part assembly built entirely from Python — the project-authoring
    path, as opposed to the bundled table."""
    parts = []
    for offset in (-6.0, 0.0, 6.0):
        part = rm.props.AssemblyPart()
        part.model = model
        part.dv = offset
        parts.append(part)
    assembly = rm.props.PropAssembly()
    assembly.id = assembly_id
    assembly.label = "Three oaks"
    assembly.parts = parts
    return assembly


# --- the catalogue -----------------------------------------------------------


def test_the_bundled_mast_arm_signal_resolves_with_its_parts():
    mast = rm.props.assembly("signal_mast")
    assert mast is not None
    assert "signal_mast" in rm.props.assembly_ids()
    assert not rm.props.is_project_assembly("signal_mast")
    assert [part.model for part in mast.parts] == [
        "pole_signal",
        "mast_arm",
        "signal_head",
        "signal_head",
    ]
    # The arm's quarter turn is what makes it an arm rather than a second pole
    # lying along the road; without it the assembly is silently wrong but valid.
    assert mast.parts[1].dyaw == pytest.approx(1.5707963267948966)
    # The two heads hang at the same height, over different lanes.
    assert mast.parts[2].dz == pytest.approx(mast.parts[3].dz)
    assert mast.parts[2].dv != pytest.approx(mast.parts[3].dv)


def test_an_unknown_assembly_id_is_none_not_an_exception():
    assert rm.props.assembly("no_such_assembly") is None


def test_the_project_overlay_adds_shadows_and_clears():
    bundled = list(rm.props.assembly_ids())

    rm.props.register_project_assemblies([_grove()])
    assert rm.props.assembly_ids() == bundled + ["my_grove"]
    assert rm.props.is_project_assembly("my_grove")

    # A project may shadow a bundled id — and must then be listed ONCE, because
    # `assembly()` resolves it to the project's copy.
    shadow = _grove(assembly_id="signal_mast")
    rm.props.register_project_assemblies([shadow])
    assert rm.props.assembly_ids().count("signal_mast") == 1
    assert rm.props.is_project_assembly("signal_mast")
    assert len(rm.props.assembly("signal_mast").parts) == 3
    # Registering REPLACES wholesale: the earlier grove is gone.
    assert "my_grove" not in rm.props.assembly_ids()

    rm.props.clear_project_assemblies()
    assert rm.props.assembly_ids() == bundled
    assert not rm.props.is_project_assembly("signal_mast")
    assert len(rm.props.assembly("signal_mast").parts) == 4


# --- placing, moving, deleting ----------------------------------------------


def test_placing_an_assembly_is_one_undo_step_and_marks_every_part():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()

    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    assert network.object_count == 4

    parts = [network.object(oid) for oid in network.objects_of(road)]
    assert all(part.assembly is not None for part in parts)
    assert {part.assembly.asset for part in parts} == {"signal_mast"}
    # One placement, one instance token, and the part indices are the catalogue's.
    assert len({part.assembly.instance for part in parts}) == 1
    assert sorted(part.assembly.part for part in parts) == [0, 1, 2, 3]
    # Distinct odr ids: minting them one at a time against the live network would
    # hand every part the same id, because none of them exists yet.
    assert len({part.odr_id for part in parts}) == 4

    stack.undo(network)
    assert network.object_count == 0
    stack.redo(network)
    assert network.object_count == 4


def test_assembly_parts_is_reachable_from_any_part_and_sorted_by_part_index():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))

    every = network.objects_of(road)
    for handle in every:
        parts = network.assembly_parts(handle)
        assert [network.object(oid).assembly.part for oid in parts] == [0, 1, 2, 3]

    # A plain prop is not a one-part assembly; the list is empty, not [itself].
    plain = rm.Object()
    plain.odr_id = "plain"
    plain.name = "tree_oak"
    plain.s = 20.0
    plain.t = 8.0
    stack.push(network, rm.edit.add_object(network, road, plain))
    standalone = [oid for oid in network.objects_of(road) if oid not in every][0]
    assert network.object(standalone).assembly is None
    assert network.assembly_parts(standalone) == []


def test_moving_the_assembly_carries_every_part_rigidly():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    parts = network.assembly_parts(network.objects_of(road)[0])
    before = [(network.object(oid).s, network.object(oid).t) for oid in parts]

    # Grab a part that is NOT the anchor: any part is a handle on the whole unit.
    stack.push(network, rm.edit.move_assembly(network, parts[2], 140.0, -6.0))
    after = [(network.object(oid).s, network.object(oid).t) for oid in parts]

    assert all(new_s == pytest.approx(old_s + 80.0) for (new_s, _), (old_s, _) in zip(after, before))
    assert all(new_t == pytest.approx(old_t) for (_, new_t), (_, old_t) in zip(after, before))

    stack.undo(network)
    assert [(network.object(oid).s, network.object(oid).t) for oid in parts] == before


def test_move_assembly_by_part_places_the_grabbed_part_not_the_anchor():
    """The drag-shaped entry point. `move_assembly` re-anchors; this one puts the
    part you grabbed where you asked. The two only agree for the anchor part,
    which is why this grabs one that is offset from it."""
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    parts = network.assembly_parts(network.objects_of(road)[0])
    grabbed = parts[2]
    before_t = network.object(grabbed).t
    assert abs(before_t - network.object(parts[0]).t) > 1.0

    stack.push(network, rm.edit.move_assembly_by_part(network, grabbed, 140.0, before_t))
    assert network.object(grabbed).s == pytest.approx(140.0)
    assert network.object(grabbed).t == pytest.approx(before_t)
    # The unit came along, still rigid.
    assert all(network.object(oid).s == pytest.approx(140.0) for oid in parts)

    # The two entry points genuinely differ — otherwise this whole function is
    # a second name for move_assembly.
    stack.undo(network)
    stack.push(network, rm.edit.move_assembly(network, grabbed, 140.0, before_t))
    assert network.object(grabbed).t != pytest.approx(before_t)


def test_deleting_through_one_part_removes_the_whole_unit_in_one_step():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    parts = network.assembly_parts(network.objects_of(road)[0])

    stack.push(network, rm.edit.delete_assembly(network, parts[1]))
    assert network.object_count == 0
    stack.undo(network)
    assert network.object_count == 4
    # Restore-in-place: the original ids come back, they are not reminted.
    assert network.assembly_parts(parts[0]) == parts


def test_a_part_cannot_be_dragged_out_of_formation_until_it_is_detached():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    parts = network.assembly_parts(network.objects_of(road)[0])
    siblings = [(oid, network.object(oid).s) for oid in parts if oid != parts[2]]

    with pytest.raises(ValueError, match="assembly part"):
        stack.push(network, rm.edit.move_object(network, parts[2], 10.0, -6.0))
    assert network.object(parts[2]).s == pytest.approx(60.0)  # refusal left it alone

    stack.push(network, rm.edit.detach_assembly_part(network, parts[2]))
    assert network.object(parts[2]).assembly is None
    stack.push(network, rm.edit.move_object(network, parts[2], 10.0, -6.0))
    assert network.object(parts[2]).s == pytest.approx(10.0)
    # The siblings stay grouped, and stay put.
    assert network.assembly_parts(parts[0]) == [oid for oid, _ in siblings]
    assert all(network.object(oid).s == pytest.approx(s) for oid, s in siblings)


# --- refusals ----------------------------------------------------------------


def test_place_assembly_refuses_whole_rather_than_placing_part_of_it():
    """Each refusal is matched on its MESSAGE, not merely on "it raised". Every
    one of these inputs is malformed in more than one way once the command starts
    unpacking it, so a bare `raises` would pass without proving which gate fired."""
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()

    with pytest.raises(ValueError, match="unknown assembly id"):
        stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "no_such"))
    assert network.object_count == 0

    # A part naming a model that does not resolve takes the whole assembly down.
    rm.props.register_project_assemblies([_grove(model="no_such_model")])
    with pytest.raises(ValueError, match="names unknown model"):
        stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "my_grove"))
    assert network.object_count == 0

    # An empty definition is not a no-op placement, it is a refusal.
    empty = rm.props.PropAssembly()
    empty.id = "empty"
    rm.props.register_project_assemblies([empty])
    with pytest.raises(ValueError, match="has no parts"):
        stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "empty"))
    assert network.object_count == 0

    # Over the bound the persistence grammar is written to: refused at author
    # time, so nothing lives in memory that the writer would have to truncate.
    huge = rm.props.PropAssembly()
    huge.id = "huge"
    huge.parts = [_grove().parts[0] for _ in range(rm.props.MAX_ASSEMBLY_PARTS + 1)]
    rm.props.register_project_assemblies([huge])
    with pytest.raises(ValueError, match="more than 16 parts"):
        stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "huge"))
    assert network.object_count == 0

    # A scale of zero would collapse every part to a point rather than fail loudly.
    rm.props.register_project_assemblies([_grove()])
    with pytest.raises(ValueError, match="scale must be finite and positive"):
        stack.push(network, rm.edit.place_assembly(network, road, 60.0, 8.0, 0.0, "my_grove", 0.0))
    assert network.object_count == 0


def test_an_assembly_that_would_hang_off_the_road_is_refused_not_clipped():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    spread = _grove()
    spread.parts[0].du = -80.0  # one part reaches a long way back along s
    rm.props.register_project_assemblies([spread])

    with pytest.raises(ValueError, match="would fall outside the road"):
        stack.push(network, rm.edit.place_assembly(network, road, 30.0, 8.0, 0.0, "my_grove"))
    assert network.object_count == 0

    # The SAME definition placed where it fits lands whole — so the refusal above
    # is about the anchor, not about the definition being unplaceable at all.
    stack.push(network, rm.edit.place_assembly(network, road, 150.0, 8.0, 0.0, "my_grove"))
    assert network.object_count == 3


# --- persistence and the obstruction rule ------------------------------------


def test_the_grouping_survives_a_round_trip_through_xodr():
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    text = rm.write_xodr(network, "assembly")
    assert 'code="rm:assembly"' in text

    reloaded, diagnostics = rm.parse_xodr(text)
    assert [d for d in diagnostics if d.severity == rm.Severity.ERROR] == []
    reloaded_road = reloaded.road_ids[0]
    parts = reloaded.assembly_parts(reloaded.objects_of(reloaded_road)[0])
    assert len(parts) == 4
    assert [reloaded.object(oid).assembly.part for oid in parts] == [0, 1, 2, 3]
    # And it is byte-identical, which is the actual never-drop contract.
    assert rm.write_xodr(reloaded, "assembly") == text


def test_an_assemblys_own_parts_are_not_reported_as_obstructing_each_other():
    """R6: the arm bolts into the pole. Interpenetration IS the assembly."""
    network = rm.RoadNetwork()
    road = _road(network)
    stack = rm.edit.EditStack()
    stack.push(network, rm.edit.place_assembly(network, road, 60.0, -6.0, 0.0, "signal_mast"))
    assert rm.validate_network(network) == []

    # The exemption is per PLACEMENT, not per asset: a second mast welded into
    # the first is still a mistake, and must still be reported.
    stack.push(network, rm.edit.place_assembly(network, road, 60.5, -6.0, 0.0, "signal_mast"))
    assert rm.validate_network(network) != []


def test_the_example_script_runs_clean(tmp_path):
    """python/examples/prop_assembly.py is the sprint's hand script."""
    example = pathlib.Path(__file__).resolve().parents[1] / "examples" / "prop_assembly.py"
    result = subprocess.run(
        [sys.executable, str(example), str(tmp_path / "prop_assembly.xodr")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "validate_network: 0 findings" in result.stdout
