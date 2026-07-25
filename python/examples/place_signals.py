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

"""Place traffic-control signals on a road (edit.add_signal / move_signal).

Signals (OpenDRIVE <signal>, section 14) are the traffic lights and signs that
control a junction. Like objects, they are added through the undoable command
layer: edit.add_signal sets signal.road and locates it by road-relative (s, t);
edit.move_signal / edit.delete_signal round-trip with the same SignalId.

Sign identities come from the shipped US pack (docs/domain/realism_defaults.md
§1.4): @country="US" with the MUTCD designation as @type. A sign's legend is
editable afterwards — edit.set_signal_text for a street name (§14 Table 122
@text) and edit.set_signal_value for a posted speed (§14.1 @value, with the
@unit the spec makes mandatory alongside it).

Run:  python place_signals.py out.xodr
"""

from __future__ import annotations

import sys

import roadmaker as rm


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "signals.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()
    stack.push(
        network,
        rm.edit.create_road([(0.0, 0.0), (100.0, 0.0)], rm.LaneProfile.two_lane_default(), ""),
    )
    road = network.find_road("1")

    # A dynamic traffic light on the right, near the road end (facing traffic).
    light = rm.Signal()
    light.odr_id = "1"
    light.type = "1000001"  # OpenDRIVE traffic-light catalog type
    light.subtype = "-1"
    light.country = "OpenDRIVE"
    light.dynamic = True
    light.s = 90.0
    light.t = -6.0
    stack.push(network, rm.edit.add_signal(network, road, light))

    # A US speed-limit sign (R2-1) on the same side, earlier along the road.
    # The posted speed is @value + @unit, not a country-coded @subtype string,
    # so it can be re-posted afterwards without retyping the sign.
    sign = rm.Signal()
    sign.odr_id = "2"
    sign.type = "R2-1"  # MUTCD designation (§14.1: @type follows @country)
    sign.subtype = "-1"
    sign.country = "US"  # ISO 3166-1 alpha-2
    sign.dynamic = False
    sign.s = 40.0
    sign.t = -6.0
    sign.width = 0.60  # §1.4 face size
    sign.height = 0.75
    stack.push(network, rm.edit.add_signal(network, road, sign))
    sign_id = next(s for s in network.signals_of(road) if network.signal(s).odr_id == "2")
    stack.push(network, rm.edit.set_signal_value(network, sign_id, 25.0, "mph"))
    assert network.signal(sign_id).value == 25.0
    assert network.signal(sign_id).unit == "mph"

    # A D3-1 street-name blade; its legend is edited afterwards with
    # set_signal_text (a single undo step) — multi-line uses a literal newline.
    plate = rm.Signal()
    plate.odr_id = "3"
    plate.type = "D3-1"
    plate.subtype = "-1"
    plate.country = "US"
    plate.dynamic = False
    plate.s = 20.0
    plate.t = 6.0
    stack.push(network, rm.edit.add_signal(network, road, plate))  # empty text…
    plate_id = next(s for s in network.signals_of(road) if network.signal(s).odr_id == "3")
    stack.push(network, rm.edit.set_signal_text(network, plate_id, "MAIN ST\nW 4TH"))
    assert network.signal(plate_id).text == "MAIN ST\nW 4TH"

    print(f"placed {network.signal_count} signals")
    assert rm.validate_network(network) == []

    # Signals render as instances of bundled signal models: a dynamic signal as
    # a traffic light, a static one as a sign. The mesh carries one signal
    # instance per placed <signal>, and the text plate carries an editable face.
    mesh = rm.build_network_mesh(network, rm.MeshOptions())
    print(f"mesh has {mesh.signal_count} signal instances")
    assert mesh.signal_count == 3

    rm.save_xodr(network, out_path, "place_signals_example")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
