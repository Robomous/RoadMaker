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

"""What an export would contain, without writing anything (p7-s1, #241).

Two previews, built on opposite principles.

`preview_xodr_export` runs the real writer in memory and counts its OUTPUT, so
it cannot disagree with what a save would produce. It also runs the full
checker sweep FIRST, which matters: `write_xodr`'s own refusal collapses to a
single message and drops every other finding.

`preview_mesh_export` cannot run its exporter (they only write to a path), so
it re-states their policy — and reports what they leave behind as carefully as
what they write. The ground channels used to be the headline example of that;
since #390 both exporters write `mesh.surfaces` and `mesh.terrain`, and the
remaining omission is a sign's text face in OpenUSD (#364).
"""

import pathlib

import roadmaker as rm

SAMPLES = pathlib.Path(__file__).resolve().parents[2] / "assets" / "samples"


def show_scene(mesh, fmt, label):
    preview = rm.preview_mesh_export(mesh, fmt)
    writable = rm.mesh_export_available(fmt)
    print(f"\n=== {label} ===")
    print(f"this build can write it: {writable}")
    if not preview.would_export:
        print(f"nothing would be exported: {preview.refusal}")

    print(f"{'channel':<18}{'in scene':>10}{'in file':>10}{'triangles':>12}  status")
    for row in preview.channels:
        if row.elements == 0:
            continue
        status = (
            "exported"
            if row.reason == rm.OmissionReason.NONE
            else str(row.reason).rsplit(".", 1)[-1].lower()
        )
        print(
            f"{row.label:<18}{row.elements:>10}{row.exported_elements:>10}"
            f"{row.triangles:>12}  {status}"
        )

    print(f"total: {preview.total_triangles} triangles, {preview.total_vertices} vertices")
    print(f"materials ({len(preview.materials)}): "
          + ", ".join(m.name for m in preview.materials[:6])
          + (" …" if len(preview.materials) > 6 else ""))
    for note in preview.notes:
        print(f"  ! {note.message}")


def main():
    network, _ = rm.load_xodr(SAMPLES / "t_junction.xodr")
    mesh = rm.build_network_mesh(network)

    # The same scene through both exporters. The triangle totals DIFFER, and
    # that is correct rather than a bug: glTF stores one shared mesh per prop
    # model and instances it with nodes, while USD bakes every instance's
    # geometry into the file.
    show_scene(mesh, rm.MeshExportFormat.GLTF, "glTF (.glb)")
    show_scene(mesh, rm.MeshExportFormat.USD, "OpenUSD (.usda)")

    # Give the scene ground, and watch the totals grow by exactly its share.
    before = rm.preview_mesh_export(mesh, rm.MeshExportFormat.GLTF)
    rm.edit.EditStack().push(network, rm.edit.create_terrain_field(network))
    grounded = rm.build_network_mesh(network)
    print("\n=== after adding a height field ===")
    preview = rm.preview_mesh_export(grounded, rm.MeshExportFormat.GLTF)
    terrain = preview.channels[int(rm.MeshChannel.TERRAIN.value)]
    print(f"terrain in the scene: {terrain.elements} field(s)")
    print(f"terrain in the file:  {terrain.exported_elements}")
    print(f"  {terrain.triangles} triangles over {terrain.vertices} vertices")
    print(
        f"  scene total {before.total_triangles} -> {preview.total_triangles} "
        f"triangles (#390)"
    )

    # And the OpenDRIVE side.
    xodr = rm.preview_xodr_export(network, "t_junction")
    print("\n=== OpenDRIVE ===")
    print(f"would write: {xodr.would_write} ({xodr.byte_count} bytes)")
    print(
        f"{xodr.road_count} roads over {xodr.total_reference_length:.1f} m, "
        f"{xodr.junction_count} junctions, {xodr.lane_count} lanes"
    )
    print(f"checker findings: {len(xodr.diagnostics)}")
    for record in xodr.rm_records:
        print(f"  rm: {record.code:<22} x{record.count}")
    if xodr.terrain_sidecar:
        # save_xodr writes this file BESIDE the .xodr; write_xodr alone knows
        # nothing about it, so the preview reads its name back out of the
        # emitted <userData code="rm:terrain"> rather than recomputing it.
        print(f"sidecar written alongside: {xodr.terrain_sidecar}")


if __name__ == "__main__":
    main()
