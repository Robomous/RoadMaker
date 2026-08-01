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

"""Road ``@rule`` (LHT/RHT) reaches Python, and decides lane travel (#535).

``<road @rule>`` is optional and §10.2 says an absent one means RHT — which is
exactly why never reading it was invisible: a left-hand-traffic network came
back as right-hand traffic on its first save, and the file still looked
well-formed.  §11 makes the rule decide the *standard* travel direction of every
lane, so this is a semantics change, not a label.
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
    / "left_hand_traffic.xodr"
)

RHT = rm.TrafficRule.RIGHT_HAND_TRAFFIC
LHT = rm.TrafficRule.LEFT_HAND_TRAFFIC


@pytest.fixture
def loaded():
    network, diagnostics = rm.load_xodr(str(CORPUS))
    return network, diagnostics


def _road(network, name):
    for road_id in network.road_ids:
        road = network.road(road_id)
        if road.name == name:
            return road
    raise AssertionError(f"road {name!r} missing from the corpus sample")


@pytest.mark.parametrize(
    ("name", "rule", "spelled"),
    [
        ("lht", LHT, "LHT"),
        ("explicit_rht", RHT, "RHT"),
        # A spelling outside e_trafficRule resolves to the spec default rather
        # than to a third state, and keeps its bytes.
        ("bogus_rule", RHT, "left"),
    ],
)
def test_rule_and_verbatim_spelling_reach_python(loaded, name, rule, spelled):
    network, _ = loaded
    road = _road(network, name)
    assert road.rule == rule
    assert road.rule_str == spelled


def test_unknown_spelling_warns_but_never_errors(loaded):
    _, diagnostics = loaded
    warnings = [d for d in diagnostics if d.severity == rm.Severity.WARNING]
    assert any("unknown road rule 'left'" in d.message for d in warnings)
    assert not any(d.severity == rm.Severity.ERROR for d in diagnostics)


@pytest.mark.parametrize(
    ("lane_odr_id", "rule", "expected"),
    [
        # Standard: the rule decides, oppositely on the two sides (§11).
        (-1, RHT, True),
        (1, RHT, False),
        (-1, LHT, False),
        (1, LHT, True),
        # Outer lanes answer like their side, not like lane +/-1 specifically.
        (-3, LHT, False),
        (3, LHT, True),
    ],
)
def test_lane_travels_with_s_follows_the_rule(lane_odr_id, rule, expected):
    assert rm.lane_travels_with_s(lane_odr_id, rm.LaneDirection.STANDARD, rule) is expected


@pytest.mark.parametrize("rule", [RHT, LHT])
@pytest.mark.parametrize("lane_odr_id", [-1, 1])
def test_reversed_inverts_whatever_the_rule_gave(lane_odr_id, rule):
    """``@direction`` overrides the rule rather than replacing it — the two
    flips compose, which is what keeps a contraflow lane describable under
    either rule."""
    standard = rm.lane_travels_with_s(lane_odr_id, rm.LaneDirection.STANDARD, rule)
    reversed_ = rm.lane_travels_with_s(lane_odr_id, rm.LaneDirection.REVERSED, rule)
    assert reversed_ is not standard


def test_round_trip_keeps_every_spelling_and_is_a_fixed_point(loaded):
    network, _ = loaded
    written = rm.write_xodr(network, "left_hand_traffic")

    # The writer omits @rule for RHT because that is what an absent attribute
    # means, so it re-emits the verbatim spelling instead. Asserting on the
    # parsed model alone would pass on a writer that dropped @rule entirely:
    # roads 2 and 3 both resolve to the value the default already gives.
    assert 'rule="LHT"' in written
    assert 'rule="RHT"' in written
    assert 'rule="left"' in written

    reloaded, _ = rm.parse_xodr(written)
    assert _road(reloaded, "lht").rule == LHT
    assert rm.write_xodr(reloaded, "left_hand_traffic") == written


def test_an_authored_road_writes_no_rule_attribute():
    """RHT is what an absent @rule means, so authoring must not start stamping
    ``rule="RHT"`` onto every file."""
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "Plain",
        "1",
    )
    assert network.road(road).rule == RHT
    assert network.road(road).rule_str == ""
    assert "rule=" not in rm.write_xodr(network, "plain")
