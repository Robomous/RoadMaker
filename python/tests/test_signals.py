"""OpenDRIVE <signals> bindings: authoring, iteration, round-trip (issue #68)."""

import pytest

import roadmaker as rm


@pytest.fixture
def network_with_road():
    network = rm.RoadNetwork()
    road_id = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_rural(),
        name="r",
    )
    return network, road_id


def make_speed_limit() -> rm.Signal:
    signal = rm.Signal()
    signal.odr_id = "1"
    signal.name = "SpeedLimit50"
    signal.s, signal.t = 50.0, -4.0
    signal.z_offset = 1.9
    signal.dynamic = False
    signal.orientation = rm.ObjectOrientation.PLUS
    signal.country, signal.country_revision = "DE", "2017"
    signal.type, signal.subtype = "274", "50"
    signal.value, signal.unit = 50.0, "km/h"
    signal.height, signal.width = 0.77, 0.77
    return signal


def test_add_lookup_erase(network_with_road):
    network, road_id = network_with_road
    signal_id = network.add_signal(road_id, make_speed_limit())
    assert signal_id
    assert network.signal_count == 1
    assert network.signals_of(road_id) == [signal_id]

    stored = network.signal(signal_id)
    assert stored.type == "274"
    assert stored.subtype == "50"
    assert stored.s == pytest.approx(50.0)
    assert stored.road == road_id
    assert stored.value == pytest.approx(50.0)

    assert network.erase_signal(signal_id)
    assert network.signal(signal_id) is None
    assert network.signal_count == 0


def test_add_to_stale_road_returns_invalid_id(network_with_road):
    network, road_id = network_with_road
    assert network.erase_road(road_id)
    assert not network.add_signal(road_id, make_speed_limit())


def test_erase_road_cascades_signals(network_with_road):
    network, road_id = network_with_road
    signal_id = network.add_signal(road_id, make_speed_limit())
    assert network.erase_road(road_id)
    assert network.signal(signal_id) is None
    assert network.signal_count == 0


def test_signals_round_trip_through_xodr(network_with_road, tmp_path):
    network, road_id = network_with_road
    network.add_signal(road_id, make_speed_limit())

    light = rm.Signal()
    light.odr_id = "2"
    light.s, light.t = 95.0, -6.0
    light.z_offset = 3.0
    light.dynamic = True
    light.orientation = rm.ObjectOrientation.PLUS
    light.country = "OpenDRIVE"
    light.type, light.subtype = "1000001", "-1"
    network.add_signal(road_id, light)

    path = tmp_path / "signals.xodr"
    rm.save_xodr(network, path, name="signals test")

    reloaded, diagnostics = rm.load_xodr(path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR]
    assert reloaded.signal_count == 2

    signals = {reloaded.signal(i).odr_id: reloaded.signal(i) for i in reloaded.signal_ids}
    assert signals["1"].type == "274"
    assert signals["1"].value == pytest.approx(50.0)
    assert signals["1"].unit == "km/h"
    assert signals["2"].dynamic is True
    assert signals["2"].country == "OpenDRIVE"


def test_validate_cites_signal_rules(network_with_road):
    network, road_id = network_with_road
    bad = rm.Signal()
    bad.odr_id = "1"
    bad.type = "274"  # no subtype, no country
    network.add_signal(road_id, bad)

    findings = rm.validate_network(network)
    assert any(f.rule_id == "asam.net:xodr:1.7.0:road.signal.signal_type" for f in findings)
    assert any(
        f.rule_id == "asam.net:xodr:1.7.0:road.signal.use_country_code" for f in findings
    )
