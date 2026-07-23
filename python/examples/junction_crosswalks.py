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

"""Add crosswalks, lane arrows and centre lines to a junction's arms.

Uses edit.junction_crosswalks, edit.junction_lane_arrows and
edit.junction_center_marks. Stop lines are NOT here: since p4-s3 (#318) every
arm already has one — they are derived, meshed and exported without anything
being authored. See junction_stoplines.py for querying and editing them.

This is the kernel geometry behind the editor's junction "Add crosswalks to all
arms" action: for each distinct arm road, derive one <object type="crosswalk">
spanning the arm's driving lanes just inside the junction, then add them as
<object>s. junction_crosswalks is pure (no mutation) — the caller adds the
objects, so an editor can group the adds into one undo step.

Run:  python junction_crosswalks.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_crosswalks.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Three straight two-lane arms meeting near the origin (a T), then a junction.
    for coords in (
        [(-40.0, 0.0), (-6.0, 0.0)],
        [(40.0, 0.0), (6.0, 0.0)],
        [(0.0, -40.0), (0.0, -6.0)],
    ):
        stack.push(network, rm.edit.create_road(coords, rm.LaneProfile.two_lane_default(), ""))
    ends = [
        rm.RoadEnd(network.find_road("1"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("2"), rm.ContactPoint.END),
        rm.RoadEnd(network.find_road("3"), rm.ContactPoint.END),
    ]
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.find_junction("1")

    # One crosswalk per arm, spanning its driving lanes. Add each as an object;
    # an editor wraps these in a single undo macro.
    crosswalks = rm.edit.junction_crosswalks(network, junction)
    print(f"authoring {len(crosswalks)} crosswalks")
    for road, crosswalk in crosswalks:
        print(
            f"  road {network.road(road).odr_id}: "
            f"s={crosswalk.s:.1f} width(across)={crosswalk.length:.1f} depth={crosswalk.width:.1f}"
        )
        stack.push(network, rm.edit.add_object(network, road, crosswalk))

    # No stop lines to author: every arm already HAS one. They are derived, so
    # they mesh and export without anything being authored — junction_stoplines
    # just reports them. See junction_stoplines.py to edit one.
    lines = rm.junction_stoplines(network, junction)
    print(f"{len(lines)} derived stop lines (nothing authored)")

    # An arrow on each approach lane, pointing into the junction. Without a
    # `glyph` callable every lane gets arrowStraight; pass one to choose the
    # turn variant per approach lane. Turn intent is the caller's — the kernel
    # does not guess it from the junction's connections.
    def glyph(road: rm.RoadId, lane_odr_id: int) -> str:
        # A toy rule: the first arm turns left, the rest keep the default.
        # Return "" to decline and take arrowStraight.
        return "arrowLeft" if road == network.find_road("1") else ""

    arrows = rm.edit.junction_lane_arrows(network, junction, glyph)
    print(f"authoring {len(arrows)} lane arrows")
    for road, arrow in arrows:
        print(f"  road {network.road(road).odr_id}: {arrow.subtype}")
        stack.push(network, rm.edit.add_object(network, road, arrow))

    # A double-yellow centre line down every arm. These are lane roadMarks, not
    # objects, so they go through set_road_mark and replace the single centre
    # line the road profile laid down.
    center_marks = rm.edit.junction_center_marks(network, junction)
    print(f"authoring {len(center_marks)} centre lines")
    for lane, mark in center_marks:
        stack.push(network, rm.edit.set_road_mark(network, lane, mark))

    assert rm.validate_network(network) == []
    rm.save_xodr(network, out_path, "junction_crosswalks_example")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
