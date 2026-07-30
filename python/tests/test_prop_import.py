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

"""Python parity for the glTF prop importer and the project overlay (p6-s8, #322)."""

from __future__ import annotations

import pathlib

import pytest
import roadmaker as rm

FIXTURES = pathlib.Path(__file__).resolve().parents[2] / "core" / "tests" / "data" / "gltf"


@pytest.fixture(autouse=True)
def _clear_overlay():
    """The overlay is process-wide, so a test that registers must not leak into the
    next one. This is the same hazard the C++ suite's fixture guards."""
    yield
    rm.props.clear_project_models()


def test_extension_predicate_matches_the_closed_format_list():
    assert rm.props.is_prop_model("chair.glb")
    assert rm.props.is_prop_model("chair.gltf")
    assert rm.props.is_prop_model("CHAIR.GLB")
    # ADR-0013's refusals.
    assert not rm.props.is_prop_model("chair.obj")
    assert not rm.props.is_prop_model("chair.fbx")
    assert not rm.props.is_prop_model("chair.usda")


def test_a_refused_format_names_the_issue_that_would_lift_it():
    with pytest.raises(RuntimeError, match="#511"):
        rm.props.import_model(FIXTURES / "chair.obj", "chair")


def test_geometry_arrives_in_the_kernel_frame_seated_on_its_base():
    model, _ = rm.props.import_model(FIXTURES / "textured_box.glb", "crate")
    assert model.id == "crate"
    assert len(model.parts) == 1
    # The glTF box is 1 x 2 x 3 with Y up, so the kernel height is the Y extent.
    assert model.height == pytest.approx(2.0, abs=1e-6)
    assert model.radius == pytest.approx(0.5 * (1.0 + 9.0) ** 0.5, abs=1e-6)

    zs = model.parts[0].positions[2::3]
    assert min(zs) == pytest.approx(0.0, abs=1e-9)
    assert max(zs) == pytest.approx(2.0, abs=1e-6)
    xs = model.parts[0].positions[0::3]
    assert min(xs) + max(xs) == pytest.approx(0.0, abs=1e-9)


def test_a_textured_material_is_flattened_and_reported():
    model, diagnostics = rm.props.import_model(FIXTURES / "textured_box.glb", "crate")
    messages = " ".join(d.message for d in diagnostics)
    assert "flattened" in messages
    assert "#507" in messages

    # The average is taken in LINEAR space, so an sRGB 0.8 channel does not
    # arrive as 0.8. If this ever reads 0.8, the transfer function is gone.
    red = model.parts[0].color[0]
    assert red == pytest.approx(((0.8 + 0.055) / 1.055) ** 2.4, abs=1e-4)
    assert red < 0.75


def test_a_factor_only_material_is_not_reported_as_flattened():
    model, diagnostics = rm.props.import_model(FIXTURES / "factor_box.glb", "plain")
    assert model.parts[0].color == pytest.approx((0.25, 0.5, 0.75), abs=1e-6)
    assert "flattened" not in " ".join(d.message for d in diagnostics)


@pytest.mark.parametrize(
    ("name", "reason"),
    [
        ("truncated.glb", "Invalid glTF binary"),
        ("bad_magic.glb", "Invalid magic"),
        ("cyclic_nodes.gltf", "cyclic"),
        ("nan_position.glb", "non-finite"),
        ("zero_height.glb", "no measurable size"),
    ],
)
def test_malformed_input_raises_with_the_reason(name, reason):
    # Refused for the STATED reason, not merely refused — two of these fixtures
    # once passed for reasons unrelated to what they cover.
    with pytest.raises(RuntimeError, match=reason):
        rm.props.import_model(FIXTURES / name, "bad")


def test_budgets_are_exposed_and_enforced():
    assert rm.props.MAX_TRIANGLES > 1000
    options = rm.props.ImportOptions()
    options.max_parts = 1
    with pytest.raises(RuntimeError, match="primitives"):
        rm.props.import_model(FIXTURES / "two_parts_and_lines.glb", "x", options)


def test_an_imported_model_joins_the_catalogue_and_leaves_on_close():
    builtin = list(rm.props.ids())
    assert rm.props.model("imported") is None

    model, _ = rm.props.import_model(FIXTURES / "textured_box.glb", "imported")
    rm.props.register_project_models([model])

    assert rm.props.model("imported") is not None
    assert rm.props.is_project_model("imported")
    assert len(rm.props.ids()) == len(builtin) + 1
    # Bundled models are untouched.
    assert rm.props.model("tree_pine") is not None

    rm.props.clear_project_models()
    assert rm.props.model("imported") is None
    assert list(rm.props.ids()) == builtin


def test_registering_replaces_rather_than_accumulating():
    first, _ = rm.props.import_model(FIXTURES / "textured_box.glb", "project_a")
    rm.props.register_project_models([first])
    second, _ = rm.props.import_model(FIXTURES / "factor_box.glb", "project_b")
    rm.props.register_project_models([second])

    # Opening project B must not leave project A's assets resolvable.
    assert rm.props.model("project_a") is None
    assert rm.props.model("project_b") is not None


def _network_with_prop(model_name: str, height: float) -> rm.RoadNetwork:
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (60.0, 0.0)], rm.LaneProfile.urban_sidewalk()
    )
    placed = rm.Object()
    placed.odr_id = "crate_1"
    placed.name = model_name  # the id is the ONLY link to the model
    placed.type = rm.ObjectType.TREE
    placed.s, placed.t = 30.0, -6.0
    placed.height = height
    network.add_object(road, placed)
    return network


def test_registering_a_model_is_what_makes_a_placed_prop_meshable():
    # NetworkMesh exposes counts rather than the instance list in Python, so this
    # asserts the observable half: whether the prop meshes at all. The scale rule
    # itself is pinned in C++ (PropOverlay.InstanceScaleWorksForAnImportedModel).
    model, _ = rm.props.import_model(FIXTURES / "textured_box.glb", "crate")
    network = _network_with_prop("crate", model.height * 3.0)

    # Before registration the model does not resolve, so nothing is instanced.
    assert rm.build_network_mesh(network).object_count == 0

    rm.props.register_project_models([model])
    assert rm.build_network_mesh(network).object_count == 1

    # And closing the project takes it away again.
    rm.props.clear_project_models()
    assert rm.build_network_mesh(network).object_count == 0


def test_an_unregistered_model_meshes_to_nothing_and_says_nothing():
    # The gap #508 tracks: a scene naming a project model, opened WITHOUT that
    # project, drops those props with no diagnostic anywhere. Pinned here so the
    # behaviour is known and the follow-up has a test to flip.
    network = _network_with_prop("not_registered", 2.0)
    assert rm.build_network_mesh(network).object_count == 0
