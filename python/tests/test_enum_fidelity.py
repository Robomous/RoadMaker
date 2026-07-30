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

"""Foreign enum spellings survive a load->save unchanged (#476).

The kernel models a subset of e_laneType / e_roadMarkType / e_roadMarkColor.
Everything outside those subsets used to be **rewritten** on save — an onRamp
lane came back as ``type="none"``, a curb mark as ``type="solid"`` — which is
worse than dropping it, because the output makes an affirmative wrong claim
about the road and nothing warns at write time.
"""

from __future__ import annotations

import pathlib
import re

import pytest
import roadmaker as rm

CORPUS = (
    pathlib.Path(__file__).resolve().parents[2]
    / "core"
    / "tests"
    / "fuzz"
    / "corpus"
    / "foreign_enum_spellings.xodr"
)


def _lane_lines(text: str) -> list[str]:
    """The `<lane>` / `<roadMark>` lines, trimmed — the granularity the defect
    lives at."""
    return [
        line.strip()
        for line in text.splitlines()
        if line.strip().startswith(("<lane ", "<roadMark "))
    ]


@pytest.fixture(scope="module")
def loaded():
    network, diagnostics = rm.parse_xodr(CORPUS.read_text())
    return network, diagnostics


def test_every_lane_and_mark_line_is_byte_identical_after_a_round_trip(loaded):
    network, _ = loaded
    written = rm.write_xodr(network, "foreign_enum_spellings")

    before = _lane_lines(CORPUS.read_text())
    after = _lane_lines(written)
    assert len(before) == len(after)
    assert len(before) >= 6, "the corpus sample must still carry every case"
    # Compared line for line rather than as a set, so a failure names the line.
    for index, (was, now) in enumerate(zip(before, after)):
        assert was == now, f"line {index} changed on write"


@pytest.mark.parametrize(
    "spelling",
    [
        'type="onRamp"',  # e_laneType we do not model — was rewritten to "none"
        'type="slipLane"',
        'type="curb"',  # e_roadMarkType we do not model — was "solid"
        'type="botts dots"',
        'color="fuchsia"',  # not an e_roadMarkColor — was "standard"
        'level="true"',  # was flattened to "false"
        'direction="backward"',  # was DELETED, and absent means "standard"
    ],
)
def test_each_unmodeled_spelling_is_re_emitted(loaded, spelling):
    """Named one by one so a regression says *which* attribute broke."""
    network, _ = loaded
    assert spelling in rm.write_xodr(network, "x")


def test_the_deprecated_sidewalk_spelling_is_not_reintroduced(loaded):
    """§11.8.1 deprecates `sidewalk` in favour of `walking`, and BOTH parse to
    LaneType.SIDEWALK — so this is a value the kernel *does* model whose spelling
    still changed on save. It is why the fix stores the spelling for every lane
    rather than only for the unmodeled ones."""
    network, _ = loaded
    written = rm.write_xodr(network, "x")
    assert 'type="walking"' in written
    assert 'type="sidewalk"' not in written


def test_preserving_the_spelling_does_not_silence_the_parser(loaded):
    """Diagnose AND keep. The user still has to learn that this build renders a
    curb mark as a generic line — the never-drop contract is not keep-quietly."""
    _, diagnostics = loaded
    messages = " | ".join(d.message for d in diagnostics if d.severity == rm.Severity.WARNING)
    for needle in ("onRamp", "slipLane", "curb", "botts dots", "fuchsia", "backward"):
        assert needle in messages
    assert [d for d in diagnostics if d.severity == rm.Severity.ERROR] == []


def test_the_spelling_is_visible_on_the_model(loaded):
    network, _ = loaded
    road = network.road(network.find_road("1"))
    section = network.lane_section(road.sections[0])
    by_id = {network.lane(l).odr_id: network.lane(l) for l in section.lanes}

    ramp = by_id[1]
    assert ramp.type == rm.LaneType.OTHER  # the enum genuinely cannot hold it…
    assert ramp.type_str == "onRamp"  # …which is exactly why the spelling is kept
    assert ramp.direction_str == "backward"
    assert ramp.level is True

    walk = by_id[2]
    assert walk.type == rm.LaneType.SIDEWALK  # modeled, but under the other spelling
    assert walk.type_str == "walking"

    centre_mark = by_id[0].road_marks[0]
    assert centre_mark.type == rm.RoadMarkType.OTHER
    assert centre_mark.type_str == "curb"
    assert centre_mark.color_str == "fuchsia"


def test_retyping_a_lane_drops_the_spelling_it_no_longer_has():
    """The trap this fix introduces if the setters are not updated: keeping the
    source spelling on a lane the user RETYPED would re-export `onRamp` for a
    lane that is now `driving` — a worse corruption, because RoadMaker would be
    authoring the lie itself."""
    network, _ = rm.parse_xodr(CORPUS.read_text())
    stack = rm.edit.EditStack()
    road = network.road(network.find_road("1"))
    section = network.lane_section(road.sections[0])
    ramp = next(l for l in section.lanes if network.lane(l).odr_id == 1)
    assert network.lane(ramp).type_str == "onRamp"

    stack.push(network, rm.edit.set_lane_type(network, ramp, rm.LaneType.DRIVING))
    assert network.lane(ramp).type_str == ""
    written = rm.write_xodr(network, "retyped")
    assert 'type="onRamp"' not in written
    assert 'type="driving"' in written

    # Undo restores the spelling with the type — the command captured both.
    stack.undo(network)
    assert network.lane(ramp).type_str == "onRamp"
    assert 'type="onRamp"' in rm.write_xodr(network, "restored")


def test_an_authored_lane_derives_its_spelling_from_the_enum():
    """Nothing RoadMaker authors carries a spelling, so authored output is
    unchanged by this fix — the byte-stability every existing fixture relies on."""
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), name="a"
    )
    section = network.lane_section(network.road(road).sections[0])
    assert all(network.lane(l).type_str == "" for l in section.lanes)
    assert all(network.lane(l).level is None for l in section.lanes)

    written = rm.write_xodr(network, "authored")
    # The enum's own names, and @level still written explicitly as before.
    assert 'type="driving"' in written
    assert re.search(r'<lane id="[^"]*" type="[a-z]+" level="false"', written)
