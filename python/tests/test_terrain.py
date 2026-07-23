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

"""The scene height field through the bindings (p5-s2, #232)."""

import pytest

import roadmaker as rm


def _straight_network():
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_rural(), "", "r0"
    )
    return net


def test_absent_field_samples_zero():
    field = rm.HeightField()
    assert field.empty
    assert rm.sample_height(field, 0.0, 0.0) == 0.0
    assert rm.sample_height(field, 1e6, -1e6) == 0.0


def test_make_flat_field_covers_the_network():
    net = _straight_network()
    field = rm.make_flat_field(net, 10.0, 50.0)
    assert not field.empty
    assert field.cols * field.rows == len(field.heights)
    assert all(z == 0.0 for z in field.heights)
    lo_x, lo_y, hi_x, hi_y = rm.field_extent(field)
    assert lo_x <= -50.0 and hi_x >= 170.0


def test_make_flat_field_is_empty_without_roads():
    assert rm.make_flat_field(rm.RoadNetwork()).empty


def test_create_and_remove_commands_with_undo():
    net = _straight_network()
    stack = rm.edit.EditStack()

    stack.push(net, rm.edit.create_terrain_field(net))
    assert not net.terrain.empty
    assert rm.build_network_mesh(net).terrain_vertex_count > 0

    stack.push(net, rm.edit.remove_terrain_field(net))
    assert net.terrain.empty

    stack.undo(net)  # undo the remove
    assert not net.terrain.empty
    stack.undo(net)  # undo the create
    assert net.terrain.empty


def test_create_rejects_an_existing_field():
    net = _straight_network()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))
    with pytest.raises(Exception):
        stack.push(net, rm.edit.create_terrain_field(net))


def test_remove_rejects_when_there_is_no_field():
    net = _straight_network()
    stack = rm.edit.EditStack()
    with pytest.raises(Exception):
        stack.push(net, rm.edit.remove_terrain_field(net))


def test_asc_sidecar_round_trips(tmp_path):
    net = _straight_network()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))

    asc = rm.write_terrain_asc(net.terrain)
    back = rm.parse_terrain_asc(asc)
    assert back.heights == net.terrain.heights
    assert back.cols == net.terrain.cols
    assert back.rows == net.terrain.rows


def test_save_writes_a_sidecar_and_load_restores_the_grid(tmp_path):
    net = _straight_network()
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))

    xodr = tmp_path / "scene.xodr"
    rm.save_xodr(net, str(xodr), "scene")
    assert (tmp_path / "scene.terrain.asc").exists()

    reloaded, _diagnostics = rm.load_xodr(str(xodr))
    assert not reloaded.terrain.empty
    assert reloaded.terrain.heights == net.terrain.heights


def test_no_field_writes_no_terrain_reference():
    net = _straight_network()
    text = rm.write_xodr(net)
    assert "rm:terrain" not in text


def test_terrain_follows_a_raised_road():
    net = _straight_network()
    road = net.road_ids[0]

    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.create_terrain_field(net))
    length = net.road(road).length
    stack.push(
        net,
        rm.edit.set_elevation_profile(
            net,
            road,
            [
                rm.edit.ElevationPoint(0.0, 0.0, 0.0),
                rm.edit.ElevationPoint(length / 2.0, 8.0, 0.0),
                rm.edit.ElevationPoint(length, 0.0, 0.0),
            ],
        ),
    )
    # The mesh buffers are not exposed to Python, but the terrain is rebuilt off
    # the raised road, so the channel stays populated.
    assert rm.build_network_mesh(net).terrain_vertex_count > 0
