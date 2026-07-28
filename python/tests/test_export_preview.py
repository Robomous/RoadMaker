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

"""Export previews from Python (p7-s1, #241)."""

import pathlib

import pytest

import roadmaker as rm

SAMPLES = pathlib.Path(__file__).resolve().parents[2] / "assets" / "samples"


@pytest.fixture
def t_junction():
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    return network


def test_scene_preview_covers_every_channel(t_junction):
    mesh = rm.build_network_mesh(t_junction)
    preview = rm.preview_mesh_export(mesh, rm.MeshExportFormat.GLTF)

    assert preview.would_export
    assert preview.refusal is None
    assert preview.total_triangles > 0
    assert preview.materials
    # Every channel is listed, including the empty and omitted ones — a
    # channel missing from the report is how #390 stayed invisible.
    assert len(preview.channels) == len(rm.MeshChannel.__members__)


def test_both_formats_are_previewable_regardless_of_the_build(t_junction):
    mesh = rm.build_network_mesh(t_junction)
    # Note there is NO skipif here, unlike test_export_usda: the manifest
    # depends on the exporter's policy, not on tinyusdz.
    usd = rm.preview_mesh_export(mesh, rm.MeshExportFormat.USD)
    assert usd.total_triangles > 0
    assert isinstance(rm.mesh_export_available(rm.MeshExportFormat.USD), bool)
    assert rm.mesh_export_available(rm.MeshExportFormat.GLTF) is True


def test_props_are_shared_in_gltf_and_baked_in_usd():
    network, _ = rm.load_xodr(SAMPLES / "tree_avenue.xodr")
    mesh = rm.build_network_mesh(network)
    gltf = rm.preview_mesh_export(mesh, rm.MeshExportFormat.GLTF)
    usd = rm.preview_mesh_export(mesh, rm.MeshExportFormat.USD)
    assert usd.total_triangles > gltf.total_triangles


def test_ground_is_reported_as_not_written(t_junction):
    rm.edit.EditStack().push(t_junction, rm.edit.create_terrain_field(t_junction))
    mesh = rm.build_network_mesh(t_junction)
    preview = rm.preview_mesh_export(mesh, rm.MeshExportFormat.GLTF)

    terrain = preview.channels[int(rm.MeshChannel.TERRAIN.value)]
    assert terrain.elements > 0, "no terrain in the fixture — the test is vacuous"
    assert terrain.exported_elements == 0
    assert terrain.reason == rm.OmissionReason.CHANNEL_NOT_WALKED
    assert "#390" in terrain.detail


# The empty-mesh refusal and the refused-WRITE finding list are pinned in C++
# (core/tests/test_export_preview.cpp): both need to mutate a NetworkMesh
# channel or a Road's lane sections in place, which the Python surface
# deliberately does not expose.


def test_the_full_checker_sweep_is_run_not_the_writers_collapsed_one(t_junction):
    """The ordering that makes the OpenDRIVE preview useful.

    write_xodr does NOT call validate_network — it calls a separate,
    hard-failing validate() that stops at the first defect. The preview must
    carry the whole advisory sweep regardless.
    """
    preview = rm.preview_xodr_export(t_junction, "t_junction")
    assert len(preview.diagnostics) == len(rm.validate_network(t_junction))


def test_xodr_preview_is_byte_identical_to_the_writer(t_junction):
    preview = rm.preview_xodr_export(t_junction, "t_junction")
    assert preview.would_write
    assert preview.xml == rm.write_xodr(t_junction, "t_junction")
    assert preview.byte_count == len(preview.xml)
    assert preview.road_count > 0
    assert preview.total_reference_length > 0.0


def test_xodr_preview_reports_layer1_and_never_layer2(t_junction):
    rm.edit.EditStack().push(t_junction, rm.edit.create_terrain_field(t_junction))
    preview = rm.preview_xodr_export(t_junction, "t_junction")

    assert preview.rm_records, "fixture emits no rm: records — the test is vacuous"
    for record in preview.rm_records:
        # Counted out of the bytes; the token carries the closing quote so
        # rm:signal is not satisfied by rm:signalmount.
        assert preview.xml.count(f'code="{record.code}"') == record.count
    # ADR-0008: the .rmscene.json companion is editor state and never appears.
    assert "rmscene" not in preview.xml

    # save_xodr writes this beside the .xodr; write_xodr alone does not know
    # about it, so the preview reads the name back out of its own output.
    assert preview.terrain_sidecar
    assert preview.terrain_sidecar in preview.xml
