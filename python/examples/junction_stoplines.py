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

"""Query and edit a junction's stop lines (p4-s3, issue #318).

Uses rm.junction_stoplines plus edit.set_stopline_distance / flip_stopline /
reset_stopline.

Stop lines are a DERIVED entity: every junction arm whose junction-facing end
has driving lanes already has one, set back 4 m and spanning the approach lanes.
Nothing has to be authored for them to mesh and export — junction_stoplines()
just reports the solve, and the three edit commands layer an override on top of
it. That is the same query the mesher and the .xodr writer read, so what you see
here is exactly what gets painted and written.

On save, each line is materialized as a self-contained
<object type="roadMark" subtype="signalLines"> — valid OpenDRIVE for any
consumer — tagged with <userData code="rm:stopline"> carrying the parametric
record. On load the tagged objects are absorbed back into the junction, so a
round trip neither duplicates them nor loses the authoring.

Run:  python junction_stoplines.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def show(network: rm.RoadNetwork, junction: rm.JunctionId, label: str) -> None:
    print(f"\n{label}")
    for info in rm.junction_stoplines(network, junction):
        arm = network.road(info.arm.road).odr_id
        direction = "outgoing" if info.flipped else "approach"
        state = "authored" if info.authored else "derived"
        print(
            f"  arm {arm}: distance={info.distance:.2f} m "
            f"(max {info.max_distance:.1f}) spanning the {direction} lanes "
            f"[{state}]"
        )


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_stoplines.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # Four straight two-lane arms meeting at the origin, then a junction. The
    # arms stop 20 m short so an authored setback stays clear of the clamp.
    for coords in (
        [(-80.0, 0.0), (-20.0, 0.0)],
        [(80.0, 0.0), (20.0, 0.0)],
        [(0.0, -80.0), (0.0, -20.0)],
        [(0.0, 80.0), (0.0, 20.0)],
    ):
        stack.push(network, rm.edit.create_road(coords, rm.LaneProfile.two_lane_default(), ""))
    ends = [
        rm.RoadEnd(network.find_road(odr), rm.ContactPoint.END) for odr in ("1", "2", "3", "4")
    ]
    stack.push(network, rm.edit.create_junction(network, ends))
    junction = network.find_junction("1")

    # 1. Four lines, with nothing authored at all.
    show(network, junction, "derived defaults — nobody authored anything:")

    # 2. Author a setback on the first arm. The value is stored UNCLAMPED and
    #    clamped to the road only when solved, so a later arm move that shortens
    #    the road never fails the mesh.
    first = rm.junction_stoplines(network, junction)[0].arm
    stack.push(network, rm.edit.set_stopline_distance(network, junction, first, 9.0))
    show(network, junction, "after set_stopline_distance(9.0) on the first arm:")

    # 3. Flip a second arm so its line spans the OUTGOING lanes instead. The
    #    command refuses a direction with no driving lanes to span.
    second = rm.junction_stoplines(network, junction)[1].arm
    stack.push(network, rm.edit.flip_stopline(network, junction, second))
    show(network, junction, "after flip_stopline on the second arm:")

    # 4. Undo/redo: every edit is one command, and reverting is exact.
    stack.undo(network)
    print("\nafter undo:", "flipped" if rm.junction_stoplines(network, junction)[1].flipped
          else "back to the approach lanes")
    stack.redo(network)
    print("after redo:", "flipped" if rm.junction_stoplines(network, junction)[1].flipped
          else "back to the approach lanes")

    # 5. reset_stopline drops an authored record and returns the arm to the
    #    derived default. Resetting an arm that authors nothing is an error.
    stack.push(network, rm.edit.reset_stopline(network, junction, first))
    show(network, junction, "after reset_stopline on the first arm:")

    # 6. Save, reload, and confirm the authoring survived the round trip.
    rm.save_xodr(network, out_path, "junction_stoplines")
    reloaded, diagnostics = rm.parse_xodr(open(out_path).read())
    assert not diagnostics, diagnostics
    show(reloaded, reloaded.find_junction("1"), f"reloaded from {out_path}:")

    exported = sum(
        1
        for line in open(out_path)
        if 'subtype="signalLines"' in line
    )
    print(f"\nwrote {out_path} — {exported} materialized stop-line objects")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
