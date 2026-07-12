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


DUPLICATE_ROAD_XML = """<OpenDRIVE>
  <header revMajor="1" revMinor="7"/>
  <road id="1" length="7">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
  <road id="1" length="5">
    <planView><geometry s="0" x="0" y="0" hdg="0" length="5"><line/></geometry></planView>
    <lanes><laneSection s="0"><center><lane id="0" type="none"/></center></laneSection></lanes>
  </road>
</OpenDRIVE>"""


def test_diagnostics_carry_rule_ids_and_entity_ids():
    network, diagnostics = rm.parse_xodr(DUPLICATE_ROAD_XML)

    # The skipped duplicate cites the uniqueness rule and, since the road was
    # never created, carries no entity id.
    duplicate = next(d for d in diagnostics if d.severity == rm.Severity.ERROR)
    assert duplicate.rule_id == "asam.net:xodr:1.4.0:ids.id_unique_in_class"
    assert not duplicate.road
    assert duplicate.rule_id in repr(duplicate)

    # The declared-length mismatch on road 1 resolves to the created road.
    mismatch = next(d for d in diagnostics if "declared length" in d.message)
    assert mismatch.rule_id == "asam.net:xodr:1.9.0:road.length_sum_geometries"
    assert mismatch.road
    assert network.road(mismatch.road).odr_id == "1"
    assert not mismatch.lane


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
    # 3 arms; the 2 connecting roads' surfaces ARE the junction floor
    # (no coplanar double-draw — issue #103).
    assert mesh.road_count == 3
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


# export_usda is only bound when the kernel is built with RM_BUILD_USD=ON.
# The published wheels ship USD-off, so skip when the symbol is absent.
@pytest.mark.skipif(not hasattr(rm, "export_usda"), reason="kernel built without RM_BUILD_USD")
def test_export_usda(tmp_path):
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    mesh = rm.build_network_mesh(network)

    out = tmp_path / "t.usda"
    rm.export_usda(mesh, out)
    text = out.read_text()
    assert text.startswith("#usda 1.0")
    assert 'upAxis = "Y"' in text
    assert 'defaultPrim = "World"' in text


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


# --- version-explicit writer + checker validation (issue #12) -----------------


def _authored_network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "Main",
        "1",
    )
    return net


def test_write_xodr_targets_opendrive_1_8_by_default():
    text = rm.write_xodr(_authored_network())
    assert 'revMajor="1"' in text
    assert 'revMinor="8"' in text


@pytest.mark.parametrize(
    ("target", "rev_minor"),
    [(rm.XodrVersion.V1_8_1, "8"), (rm.XodrVersion.V1_9_0, "9")],
)
def test_write_xodr_target_version_selects_the_header(target, rev_minor):
    text = rm.write_xodr(_authored_network(), target_version=target)
    assert f'revMinor="{rev_minor}"' in text


def test_save_xodr_reload_is_byte_equal(tmp_path):
    # Python parity of the Phase 1 gate: author -> save -> reload -> byte-equal.
    path = tmp_path / "scene.xodr"
    rm.save_xodr(_authored_network(), path, "scene")

    reloaded, diagnostics = rm.load_xodr(path)
    assert not diagnostics
    assert rm.write_xodr(reloaded, "scene") == path.read_text()


def test_validate_network_is_quiet_on_a_valid_network():
    net = _authored_network()
    assert rm.validate_network(net) == []
    assert rm.validate_network(net, target_version=rm.XodrVersion.V1_9_0) == []


def test_validate_network_cites_version_specific_rules_only_for_their_target():
    # A two-arm junction violates junctions.common.not_only_two — a rule that
    # exists only in the 1.9.0 catalog (Annex F.4.5.3), so targeting 1.8.1
    # has nothing to cite. Two short arms meeting head-on stay within the
    # generator's 50 m end-distance limit.
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net, [(-40.0, 0.0), (-6.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "1")
    rm.author_clothoid_road(
        net, [(40.0, 0.0), (6.0, 0.0)], rm.LaneProfile.two_lane_default(), "", "2")
    stack = rm.edit.EditStack()
    stack.push(
        net,
        rm.edit.create_junction(
            net,
            [
                rm.RoadEnd(net.find_road("1"), rm.ContactPoint.END),
                rm.RoadEnd(net.find_road("2"), rm.ContactPoint.END),
            ],
        ),
    )

    not_only_two = "asam.net:xodr:1.9.0:junctions.common.not_only_two"
    # A common junction always draws the boundary-omitted warning (M2 emits the
    # surface without <boundary>), so filter by rule rather than expecting an
    # empty finding set: not_only_two is cited only for the 1.9.0 target.
    assert not_only_two not in [f.rule_id for f in rm.validate_network(net)]
    findings = rm.validate_network(net, target_version=rm.XodrVersion.V1_9_0)
    matched = [f for f in findings if f.rule_id == not_only_two]
    assert len(matched) == 1
    assert matched[0].severity == rm.Severity.WARNING
