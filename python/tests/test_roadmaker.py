"""pytest suite for the roadmaker Python package."""

from pathlib import Path

import pytest

import roadmaker as rm

SAMPLES = Path(__file__).resolve().parents[2] / "assets" / "samples"


def test_version():
    assert rm.version() == rm.__version__
    assert rm.version().count(".") == 2


def test_load_sample_straight_road():
    network, diagnostics = rm.load_xodr(SAMPLES / "straight_road.xodr")
    assert network.road_count == 1
    assert all(d.severity != rm.Severity.ERROR for d in diagnostics)

    road = network.road(network.find_road("1"))
    assert road.name == "Main Street"
    assert road.length == pytest.approx(100.0)

    section = network.lane_section(road.sections[0])
    lane_ids = [network.lane(lane).odr_id for lane in section.lanes]
    assert lane_ids == [2, 1, 0, -1, -2]  # leftmost first

    sidewalk = network.lane(section.lanes[0])
    assert sidewalk.type == rm.LaneType.SIDEWALK
    assert sidewalk.widths[0].a == pytest.approx(2.0)


def test_reference_line_evaluate():
    network, _ = rm.load_xodr(SAMPLES / "curved_road.xodr")
    road = network.road(network.find_road("1"))
    assert road.plan_view.record_count == 4

    start = road.plan_view.evaluate(0.0)
    assert (start.x, start.y) == (pytest.approx(0.0), pytest.approx(0.0))
    # Inside the arc segment curvature is 0.05.
    mid = road.plan_view.evaluate(60.0)
    assert mid.curvature == pytest.approx(0.05, abs=1e-9)


def test_junction_topology():
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    assert network.junction_count == 1
    junction = network.junction(network.find_junction("100"))
    assert len(junction.connections) == 2
    assert junction.connections[0].lane_links == [(-1, -1)]

    through = network.road(network.find_road("10"))
    assert through.junction  # connecting road belongs to the junction


def test_missing_file_raises_file_not_found():
    with pytest.raises(FileNotFoundError):
        rm.load_xodr(SAMPLES / "nope.xodr")


def test_malformed_xml_raises_value_error():
    with pytest.raises(ValueError):
        rm.parse_xodr("<OpenDRIVE><road")


def test_author_and_round_trip():
    network = rm.RoadNetwork()
    waypoints = [(0.0, 0.0), (50.0, 10.0), (90.0, 50.0)]
    road_id = rm.author_clothoid_road(
        network, waypoints, rm.LaneProfile.two_lane_default(), name="Py Road"
    )
    assert road_id.valid
    road = network.road(road_id)
    assert road.length > 100.0

    xml = rm.write_xodr(network, name="pytest")
    assert "<OpenDRIVE" in xml and "<geometry" in xml

    reparsed, diagnostics = rm.parse_xodr(xml)
    assert reparsed.road_count == 1
    assert all(d.severity != rm.Severity.ERROR for d in diagnostics)
    round_road = reparsed.road(reparsed.find_road(road.odr_id))
    assert round_road.length == pytest.approx(road.length, abs=1e-4)

    # Dense geometric comparison within round-trip tolerances.
    for i in range(0, 101):
        s = road.length * i / 100
        a = road.plan_view.evaluate(s)
        b = round_road.plan_view.evaluate(s)
        assert a.x == pytest.approx(b.x, abs=1e-4)
        assert a.y == pytest.approx(b.y, abs=1e-4)
        assert a.hdg == pytest.approx(b.hdg, abs=1e-6)


def test_authoring_rejects_bad_input():
    network = rm.RoadNetwork()
    with pytest.raises(ValueError):
        rm.author_clothoid_road(network, [(0, 0)], rm.LaneProfile.two_lane_default())
    with pytest.raises(ValueError):
        rm.author_clothoid_road(network, [(0, 0), (0, 0)], rm.LaneProfile.two_lane_default())


def test_export_glb(tmp_path):
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    mesh = rm.build_network_mesh(network)
    assert mesh.road_count == 5
    assert mesh.junction_floor_count == 1
    assert mesh.vertex_count > 0

    out = tmp_path / "t.glb"
    rm.export_glb(mesh, out)
    data = out.read_bytes()
    assert data[:4] == b"glTF"  # binary glTF magic


def test_export_empty_mesh_raises():
    mesh = rm.build_network_mesh(rm.RoadNetwork())
    with pytest.raises(ValueError):
        rm.export_glb(mesh, "unused.glb")


def test_network_editing_api():
    network = rm.RoadNetwork()
    road_id = network.create_road("r", "1")
    section = network.add_lane_section(road_id, 0.0)
    lane = network.add_lane(section, -1, rm.LaneType.DRIVING)
    assert lane.valid
    assert network.lane_count == 1

    assert network.erase_road(road_id)
    assert network.road(road_id) is None
    assert network.lane(lane) is None  # cascaded
