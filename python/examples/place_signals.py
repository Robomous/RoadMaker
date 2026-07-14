"""Place traffic-control signals on a road (edit.add_signal / move_signal).

Signals (OpenDRIVE <signal>, section 14) are the traffic lights and signs that
control a junction. Like objects, they are added through the undoable command
layer: edit.add_signal sets signal.road and locates it by road-relative (s, t);
edit.move_signal / edit.delete_signal round-trip with the same SignalId.

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

    # A static speed-limit (50) sign on the same side, earlier along the road.
    sign = rm.Signal()
    sign.odr_id = "2"
    sign.type = "274"
    sign.subtype = "50"
    sign.country = "DE"
    sign.dynamic = False
    sign.s = 40.0
    sign.t = -6.0
    stack.push(network, rm.edit.add_signal(network, road, sign))

    print(f"placed {network.signal_count} signals")
    assert rm.validate_network(network) == []

    # Signals render as instances of bundled signal models: a dynamic signal as
    # a traffic light, a static one as a sign. The mesh carries one signal
    # instance per placed <signal> (the light + the speed-limit sign here).
    mesh = rm.build_network_mesh(network, rm.MeshOptions())
    print(f"mesh has {mesh.signal_count} signal instances")
    assert mesh.signal_count == 2

    rm.save_xodr(network, out_path, "place_signals_example")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
