#!/usr/bin/env python3
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

"""Importing your own 3D model as a prop, headless (p6-s8, #322).

glTF and GLB are read with the tinygltf already in the tree, so this costs no new
dependency; OBJ needs a pinned one (#511) and USD read is out for v0.1.0, both
refused by name. FBX is permanently excluded. ADR-0013 records all of it.

TWO THINGS TO KNOW BEFORE YOU IMPORT ANYTHING:

1. A TEXTURED MODEL IMPORTS FLAT. A prop mesh carries one colour per part and no
   UVs, so a baseColorTexture is decoded, averaged, and reported as flattened.
   #507 is the issue that changes that. The average is computed in LINEAR space,
   which is why a mid-grey texture does not import as mid-grey.

2. THE ID IS THE CONTRACT. It becomes the OpenDRIVE `<object @name>` of every
   placed instance, and it is the ONLY link between an object and its model. A
   scene using an imported prop, opened without the project that holds it, draws
   nothing where those props were (#508 tracks saying so out loud). So ids must be
   unique and stable across sessions -- the editor derives them from the asset
   slug in the project's library manifest.

Run:  python3 python/examples/prop_import.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

FIXTURES = pathlib.Path(__file__).resolve().parents[2] / "core" / "tests" / "data" / "gltf"


def main() -> int:
    # 1. What the reader will accept. Ask rather than guess: this is the same
    #    predicate a file dialog filters on, so the two cannot disagree.
    print("1. Accepted extensions")
    for name in ("chair.glb", "chair.gltf", "chair.obj", "chair.fbx", "chair.usda"):
        print(f"   {name:14s} {'yes' if rm.props.is_prop_model(name) else 'no'}")

    print("\n   budgets, stated rather than discovered:")
    print(f"     triangles    {rm.props.MAX_TRIANGLES:,}")
    print(f"     vertices     {rm.props.MAX_VERTICES:,}")
    print(f"     parts        {rm.props.MAX_PARTS:,}")
    print(f"     image texels {rm.props.MAX_IMAGE_TEXELS:,}")

    # 2. Read a model. The geometry arrives already rotated into the kernel frame
    #    (Y-up -> Z-up) and reseated so its base sits at z = 0 with its horizontal
    #    centre at the origin -- the contract every placement path relies on.
    print("\n2. Import a textured box")
    model, diagnostics = rm.props.import_model(FIXTURES / "textured_box.glb", "my_crate")
    print(f"   id      {model.id}")
    print(f"   parts   {len(model.parts)}")
    print(f"   height  {model.height:.3f} m")
    print(f"   radius  {model.radius:.3f} m")
    for part in model.parts:
        r, g, b = part.color
        triangles = len(part.indices) // 3
        print(f"     part {part.name!r}: {triangles} triangles, linear rgb "
              f"({r:.3f}, {g:.3f}, {b:.3f})")

    # 3. Read the diagnostics. The flatten notice lives here rather than being
    #    raised, because losing a texture is not a reason to refuse a model.
    print("\n3. Diagnostics")
    for diagnostic in diagnostics:
        print(f"   [{diagnostic.severity}] {diagnostic.location}: {diagnostic.message}")

    # 4. Make it resolvable. Until this call the model is just data; after it,
    #    props.model() answers for it exactly as it does for a bundled prop, and
    #    so do the mesh builder and both exporters.
    print("\n4. Register it as the open project's asset")
    before = len(rm.props.ids())
    rm.props.register_project_models([model])
    print(f"   catalogue {before} -> {len(rm.props.ids())}")
    print(f"   resolves:        {rm.props.model('my_crate') is not None}")
    print(f"   is project asset: {rm.props.is_project_model('my_crate')}")
    print(f"   bundled still there: {rm.props.model('tree_pine') is not None}")

    # 5. Place it. From here nothing is import-specific: this is the ordinary
    #    object-authoring path, and @height is what per-instance sizing divides by.
    print("\n5. Place one on a road at twice its authored size")
    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network,
        [(0.0, 0.0), (60.0, 0.0)],
        rm.LaneProfile.urban_sidewalk(),
        name="Prop Import Demo",
    )
    placed = rm.Object()
    placed.odr_id = "crate_1"
    placed.name = "my_crate"          # the id -- the only link to the model
    placed.type = rm.ObjectType.TREE  # what the model declares
    placed.s = 30.0
    placed.t = -6.0
    placed.height = model.height * 2.0
    placed.radius = model.radius * 2.0
    network.add_object(road, placed)

    mesh = rm.build_network_mesh(network)
    print(f"   instanced props in the mesh: {mesh.object_count}")
    print(f"   declared height {placed.height:.2f} m over authored {model.height:.2f} m "
          f"-> render scale {placed.height / model.height:.2f}")
    print("   (an unregistered id would mesh to 0 here, silently — see #508)")

    # 6. Closing a project drops the overlay wholesale, so one project's assets
    #    are never resolvable inside another.
    print("\n6. Close the project")
    rm.props.clear_project_models()
    print(f"   resolves: {rm.props.model('my_crate') is not None}")
    print(f"   catalogue back to {len(rm.props.ids())}")

    # 7. A malformed file is refused with a reason, never a crash.
    print("\n7. Refusals name what went wrong")
    for name in ("truncated.glb", "cyclic_nodes.gltf", "zero_height.glb"):
        try:
            rm.props.import_model(FIXTURES / name, "bad")
        except RuntimeError as error:
            print(f"   {name:20s} {error}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
