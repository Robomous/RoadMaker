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

"""The OpenSCENARIO DSL export through the bindings (issue #327).

Export-only by design: there is no `rm.osc.parse_osc2`, and its absence is
asserted here so a future addition is a deliberate act rather than a drift.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

import roadmaker as rm


@pytest.fixture
def scenario() -> rm.osc.Scenario:
    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()
    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, "town.xodr"))
    position = rm.osc.WorldPosition()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), position
        ),
    )
    stack.push(scenario, rm.osc.edit.set_entity_init_speed(scenario, "Ego", 13.89))
    return scenario


def test_the_emitted_text_is_the_specifications_concrete_shape(scenario):
    text = rm.osc.write_osc2(scenario)

    assert "import osc.standard.all" in text
    assert "basic.osc" not in text, "there is no basic.osc; the library files are types/domain/standard"
    assert "scenario top:" in text
    assert 'map.set_map_file("town.xodr")' in text
    assert "ego: vehicle" in text
    assert "keep(it.vehicle_category == car)" in text
    assert "do parallel:" in text
    # `mps` is a normative unit, so the kernel's m/s needs no conversion.
    assert "speed(13.89mps)" in text
    assert "kph" not in text


def test_the_version_is_named_rather_than_two_dot_x(scenario):
    assert rm.osc.OSC2_VERSION == "2.2.0"
    assert rm.osc.OSC2_EXTENSION == ".osc"
    assert rm.osc.OSC2_VERSION in rm.osc.write_osc2(scenario)


def test_the_output_is_deterministic(scenario):
    assert rm.osc.write_osc2(scenario) == rm.osc.write_osc2(scenario)


def test_the_scenario_name_is_settable(scenario):
    assert "scenario cut_in:" in rm.osc.write_osc2(scenario, "cut_in")


def test_save_writes_the_same_bytes(scenario, tmp_path):
    path = tmp_path / "s.osc"
    rm.osc.save_osc2(scenario, str(path))
    assert path.read_text() == rm.osc.write_osc2(scenario)


def test_there_is_no_importer_and_that_is_the_design():
    """★ Export-only is a promise, so its absence is asserted rather than assumed."""
    assert not hasattr(rm.osc, "parse_osc2")
    assert not hasattr(rm.osc, "load_osc2")


def test_the_subset_registry_is_reachable_and_non_empty():
    supported = rm.osc.osc2_supported()
    unsupported = rm.osc.osc2_unsupported()
    assert supported and unsupported
    assert any("map.set_map_file" in row.construct for row in supported)
    assert any("Story" in row.construct for row in unsupported)
    # Every row explains itself; a reason-less row is documentation of nothing.
    for row in list(supported) + list(unsupported):
        assert row.source.strip()


def test_what_the_subset_drops_is_reported(scenario):
    story = rm.osc.Story()
    story.name = "s"
    action = rm.osc.Action()
    action.name = "a"
    event = rm.osc.Event()
    event.name = "e"
    event.actions = [action]
    maneuver = rm.osc.StoryManeuver()
    maneuver.name = "m"
    maneuver.events = [event]
    group = rm.osc.ManeuverGroup()
    group.name = "g"
    group.maneuvers = [maneuver]
    act = rm.osc.Act()
    act.name = "act"
    act.maneuver_groups = [group]
    story.acts = [act]
    rm.osc.edit.ScenarioStack().push(scenario, rm.osc.edit.set_story(scenario, 0, story))

    findings = rm.osc.validate_osc2_subset(scenario)
    assert any("<Story>" in f.message for f in findings)
    # Warnings, never errors: the file is lossy by design and still written.
    assert all(f.severity == rm.Severity.WARNING for f in findings)
    assert rm.osc.write_osc2(scenario)


def test_two_names_that_collapse_to_one_identifier_are_refused(scenario):
    stack = rm.osc.edit.ScenarioStack()
    position = rm.osc.WorldPosition()
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "ego"), position
        ),
    )
    with pytest.raises(ValueError, match="identifier"):
        rm.osc.write_osc2(scenario)


def test_the_example_script_runs_clean(tmp_path):
    """python/examples/scenario_osc2_export.py is this sprint's hand script."""
    example = (
        pathlib.Path(__file__).resolve().parents[1] / "examples" / "scenario_osc2_export.py"
    )
    result = subprocess.run(
        [sys.executable, str(example), "--out", str(tmp_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    # Both files, from one model — the point of the script.
    assert (tmp_path / "crossing.xosc").exists()
    assert (tmp_path / "crossing.osc").exists()
    assert "not carried into the 2.x file" in result.stdout
