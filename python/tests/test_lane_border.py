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

"""Border-authored lane geometry survives a load (§11.7.2, #538).

`<border>` is OpenDRIVE's alternative encoding of lane geometry — the lane's
outer t-limit instead of its width. It used to be warned about once and then
dropped, and the lane preserved tier excluded it too, so a border-authored lane
arrived with **no widths at all**: zero width in the mesh, and written back
width-less. That is geometry loss, not markup loss.
"""

from __future__ import annotations

import pathlib

import pytest
import roadmaker as rm

CORPUS = (
    pathlib.Path(__file__).resolve().parents[2]
    / "core"
    / "tests"
    / "fuzz"
    / "corpus"
    / "lane_border.xodr"
)


@pytest.fixture
def loaded():
    return rm.load_xodr(str(CORPUS))


def _lane(network, road_odr_id, lane_odr_id):
    for road_id in network.road_ids:
        road = network.road(road_id)
        if road.odr_id != road_odr_id:
            continue
        section = network.lane_section(road.sections[0])
        for lane_id in section.lanes:
            lane = network.lane(lane_id)
            if lane.odr_id == lane_odr_id:
                return lane
    raise AssertionError(f"lane {lane_odr_id} of road {road_odr_id!r} missing")


def _width_at(lane, s):
    """Evaluate the piecewise-cubic width profile the way the kernel does."""
    covering = None
    for poly in lane.widths:
        if poly.s <= s + 1e-9:
            covering = poly
    if covering is None:
        covering = lane.widths[0]
    ds = s - covering.s
    return covering.a + ds * (covering.b + ds * (covering.c + ds * covering.d))


@pytest.mark.parametrize("lane_odr_id", [2, 1, -1, -2, -3])
def test_every_bordered_lane_has_geometry(loaded, lane_odr_id):
    network, _ = loaded
    assert _lane(network, "1", lane_odr_id).widths, "the lane lost its geometry entirely"


def test_widths_are_the_gap_between_neighbouring_borders(loaded):
    network, _ = loaded
    # The outer lane's border is 7.0, but its WIDTH is 7.0 - 3.5.
    assert _width_at(_lane(network, "1", 1), 0.0) == pytest.approx(3.5)
    assert _width_at(_lane(network, "1", 2), 0.0) == pytest.approx(3.5)
    assert _width_at(_lane(network, "1", -1), 0.0) == pytest.approx(3.5)


def test_a_lane_breaks_where_its_inner_neighbour_does(loaded):
    """Lane -3's own border never breaks; lane -2's does at s=50. Lane -3 must
    still narrow past that station as lane -2 widens beneath it."""
    network, _ = loaded
    shoulder = _lane(network, "1", -3)
    assert _width_at(shoulder, 0.0) == pytest.approx(2.0)
    assert _width_at(shoulder, 50.0) == pytest.approx(2.0)
    assert _width_at(shoulder, 100.0) == pytest.approx(1.0)
    assert _width_at(_lane(network, "1", -2), 100.0) == pytest.approx(4.5)


def test_width_wins_when_a_lane_declares_both(loaded):
    """§11.7.2: "the application shall use the information from the <width>
    elements". The fixture's border is absurd so a build that used it fails."""
    network, diagnostics = loaded
    assert _width_at(_lane(network, "2", -1), 0.0) == pytest.approx(3.0)
    assert any("declares both <width> and <border>" in d.message for d in diagnostics)


def test_the_round_trip_keeps_the_geometry_and_emits_one_encoding(loaded):
    network, _ = loaded
    written = rm.write_xodr(network, "lane_border")

    # The ENCODING changes by design — borders become widths — but a border
    # must never be re-emitted beside the width it became; §11.7.2 makes the
    # two mutually exclusive.
    assert "<width" in written
    assert "<border" not in written

    reloaded, _ = rm.parse_xodr(written)
    for lane_odr_id in (2, 1, -1, -2, -3):
        before = _lane(network, "1", lane_odr_id)
        after = _lane(reloaded, "1", lane_odr_id)
        for s in (0.0, 25.0, 50.0, 75.0, 100.0):
            assert _width_at(after, s) == pytest.approx(_width_at(before, s)), (
                f"lane {lane_odr_id} changed width at s={s}"
            )

    assert rm.write_xodr(reloaded, "lane_border") == written


def test_converted_lanes_are_not_reported_as_missing_geometry(loaded):
    _, diagnostics = loaded
    assert not any("non-center lane without <width>" in d.message for d in diagnostics)
    assert not any(d.severity == rm.Severity.ERROR for d in diagnostics)
