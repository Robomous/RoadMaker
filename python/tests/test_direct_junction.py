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

"""Direct and crossing junctions survive a round trip (§12.4/§12.8, #534).

A direct junction has no connecting road — each ``<connection>`` carries
``@linkedRoad``.  The reader read ``@type`` only to test ``== "virtual"`` and
read every connection expecting ``@connectingRoad``, so the type was dropped
*and* every connection was skipped as "unknown road ''".  Saving produced an
empty common junction: the topology destroyed, not merely degraded.
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
    / "direct_junction.xodr"
)


@pytest.fixture
def loaded():
    return rm.load_xodr(str(CORPUS))


def test_the_direct_junction_keeps_its_type_and_connections(loaded):
    network, _ = loaded
    junction = network.junction(network.find_junction("100"))
    assert junction.type == rm.JunctionType.DIRECT
    assert len(junction.direct_connections) == 2
    # They are NOT ordinary connections: a direct junction has no connecting
    # road, so the derived machinery must find nothing to build.
    assert junction.connections == []


def test_linked_road_and_overlap_zone_reach_python(loaded):
    network, _ = loaded
    junction = network.junction(network.find_junction("100"))
    linked = [network.road(c.linked_road).odr_id for c in junction.direct_connections]
    assert linked == ["2", "3"]

    link = junction.direct_connections[1].lane_links[0]
    assert (link.from_lane, link.to_lane) == (-2, -1)
    assert link.overlap_zone == pytest.approx(45.0)
    assert link.from_layer == "permanent"
    # Absent stays absent — the spec's default of 100 is never materialised.
    assert junction.direct_connections[0].lane_links[0].overlap_zone is None


def test_a_crossing_junction_uses_the_ordinary_connection_path(loaded):
    """Not everything that is not `default` is `direct`: §12.8 crossing
    junctions use @connectingRoad like a common one."""
    network, _ = loaded
    junction = network.junction(network.find_junction("200"))
    assert junction.type == rm.JunctionType.CROSSING
    assert junction.direct_connections == []
    assert len(junction.connections) == 1


def test_no_misleading_unknown_road_diagnostic(loaded):
    _, diagnostics = loaded
    assert not any("unknown road ''" in d.message for d in diagnostics)
    assert not any(d.severity == rm.Severity.ERROR for d in diagnostics)


def test_the_round_trip_is_a_fixed_point(loaded):
    network, _ = loaded
    written = rm.write_xodr(network, "direct_junction")
    assert 'type="direct"' in written
    assert 'type="crossing"' in written
    assert 'linkedRoad="2"' in written
    assert 'overlapZone="45"' in written

    reloaded, _ = rm.parse_xodr(written)
    assert reloaded.junction(reloaded.find_junction("100")).type == rm.JunctionType.DIRECT
    assert rm.write_xodr(reloaded, "direct_junction") == written
