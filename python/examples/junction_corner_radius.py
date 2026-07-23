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

"""Author junction corner fillets (roadmaker.junction_corners + edit.set_corner_*).

Every corner of a junction — the re-entrant pocket between two angularly
adjacent arms — is DERIVED by default: the mesher fillets it with a radius read
off the crossing connecting road. `junction_corners` exposes that solve (the
same one the mesher, the editor's Corner tool and the properties pane share),
and `edit.set_corner_radius` / `edit.set_corner_extents` author a sparse
override for one named corner. Overrides persist as
`<userData code="rm:corners">` on `<junction>` — ASAM OpenDRIVE 1.9.0 §12.10
gives `<boundary>` no corner-radius carrier — while the exported
`<boundary>`/`<elevationGrid>` stay fully derived.

Run:  python junction_corner_radius.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm

assembly = rm.edit.assembly


def describe(network: rm.RoadNetwork, junction: rm.JunctionId, title: str) -> None:
    print(title)
    for index, corner in enumerate(rm.junction_corners(network, junction)):
        flag = "authored" if corner.radius_authored else "derived "
        if corner.extents_authored:
            flag = "extents "
        print(
            f"  corner {index}: {flag} radius={corner.radius:6.2f} m "
            f"(max {corner.max_radius:5.2f}) legs=({corner.extent_a:5.2f}, "
            f"{corner.extent_b:5.2f}) curve={len(corner.curve)} pts "
            f"apex=({corner.apex()[0]:7.2f}, {corner.apex()[1]:7.2f})"
        )


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_corner_radius.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # A 4-way crossing at the origin. A generous junction gap leaves the arm
    # faces room, so the derived fillet is well under its geometric maximum.
    params = assembly.IntersectionParams()
    params.gap_m = 24.0
    stack.push(network, assembly.x_intersection(network, assembly.Pose(0.0, 0.0, 0.0), params))
    junction = network.junction_ids[0]

    describe(network, junction, "derived corners:")

    # Author a larger radius on the first corner. The corner is named by its
    # adjacent arm pair, not by index — the identity survives a regenerate.
    first = rm.junction_corners(network, junction)[0]
    target = 0.5 * (first.radius + first.max_radius)
    stack.push(
        network, rm.edit.set_corner_radius(network, junction, first.arm_a, first.arm_b, target)
    )
    describe(network, junction, f"\nafter set_corner_radius({target:.2f}):")
    print(f"  Junction.corners = {network.junction(junction).corners}")

    # Undo restores the derived fillet exactly (byte-identical .xodr); redo
    # re-applies it.
    stack.undo(network)
    assert not rm.junction_corners(network, junction)[0].radius_authored
    assert network.junction(junction).corners == []
    stack.redo(network)
    assert rm.junction_corners(network, junction)[0].radius_authored

    # Per-side reach: the two tangent legs can differ, and the corner curve
    # stays G1-tangent to both edges (a rational quadratic Bezier), so an
    # asymmetric corner is still watertight.
    stack.push(
        network, rm.edit.set_corner_extents(network, junction, first.arm_a, first.arm_b, 6.0, 12.0)
    )
    describe(network, junction, "\nafter set_corner_extents(6, 12):")

    findings = rm.validate_network(network)
    errors = [f for f in findings if f.severity == rm.Severity.ERROR]
    print(f"\nvalidation: {len(findings)} findings, {len(errors)} errors")
    assert not errors, "an authored corner must not break OpenDRIVE validity"

    rm.save_xodr(network, out_path, "junction_corner_radius_example")

    # The override survives the file: rm:corners is read back into
    # Junction.corners and the solve reproduces the authored geometry.
    reloaded, diagnostics = rm.load_xodr(out_path)
    assert not [d for d in diagnostics if d.severity == rm.Severity.ERROR], diagnostics
    restored = reloaded.junction(reloaded.junction_ids[0]).corners
    print(f"wrote {out_path}; reloaded rm:corners = {restored}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
