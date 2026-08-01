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

"""Road @rule (LHT/RHT) — ASAM OpenDRIVE 1.9.0 §10.2, and what it decides (§11).

`<road @rule>` is one optional attribute with two legal spellings, and §10.2
says an absent one means RHT.  That default is exactly why dropping it was
invisible: before #535 the reader never looked at `@rule`, so a left-hand-traffic
network came back as RIGHT-hand traffic on its first save, and the output looked
perfectly well-formed while making the opposite claim about which way traffic
runs.

It is not a label.  §11 defines the *standard* driving direction of a lane as a
function of the rule AND the lane's <left>/<right> grouping:

    RHT -> <right> lanes (negative @id) run with +s, <left> against it
    LHT -> <left>  lanes (positive @id) run with +s, <right> against it

`@direction` on a lane then overrides whatever that gives.  `lane_travels_with_s`
is the kernel's single definition of the composition, and the route resolver,
signal facing and junction-arm lane selection all route through it.

Run:  python python/examples/road_rule.py
"""

from __future__ import annotations

import pathlib

import roadmaker as rm

REPO = pathlib.Path(__file__).resolve().parents[2]
CORPUS = REPO / "core" / "tests" / "fuzz" / "corpus" / "left_hand_traffic.xodr"


def rule_name(rule: rm.TrafficRule) -> str:
    return "LHT" if rule == rm.TrafficRule.LEFT_HAND_TRAFFIC else "RHT"


def main() -> None:
    network, diagnostics = rm.load_xodr(str(CORPUS))
    print(f"=== {CORPUS.name} ===")

    for road_id in network.road_ids:
        road = network.road(road_id)
        # `rule` is the resolved enum; `rule_str` is what the file actually
        # spelled, which is empty for an authored road and may be a spelling
        # outside e_trafficRule for a foreign one.
        spelled = road.rule_str or "(absent)"
        print(f"  {road.name!r}: rule={rule_name(road.rule)}  as spelled: {spelled!r}")

    for d in diagnostics:
        # An unknown spelling resolves to the spec default AND warns. It is
        # never silently dropped, and never an error: the file still loads.
        print(f"  [{d.severity}] {d.message}")

    # What the rule actually decides. Same lane id, same (absent) @direction --
    # only the road's rule differs, and the answer inverts.
    print("\nStandard travel direction, by rule (True = travels toward +s):")
    for lane_id in (-1, 1):
        for rule in (rm.TrafficRule.RIGHT_HAND_TRAFFIC, rm.TrafficRule.LEFT_HAND_TRAFFIC):
            travels = rm.lane_travels_with_s(lane_id, rm.LaneDirection.STANDARD, rule)
            print(f"  lane {lane_id:>2}  under {rule_name(rule)}  -> {travels}")

    # @direction overrides the rule rather than replacing it: the two flips
    # compose, so a reversed lane on an LHT road lands back on the RHT answer.
    assert rm.lane_travels_with_s(-1, rm.LaneDirection.STANDARD, rm.TrafficRule.RIGHT_HAND_TRAFFIC)
    assert not rm.lane_travels_with_s(-1, rm.LaneDirection.STANDARD, rm.TrafficRule.LEFT_HAND_TRAFFIC)
    assert rm.lane_travels_with_s(-1, rm.LaneDirection.REVERSED, rm.TrafficRule.LEFT_HAND_TRAFFIC)

    # Round trip. The writer omits @rule for RHT, since that is what an absent
    # attribute means -- so it re-emits the VERBATIM spelling instead, which is
    # what keeps an explicit rule="RHT" explicit and an unknown spelling intact.
    written = rm.write_xodr(network, "left_hand_traffic")
    assert 'rule="LHT"' in written, "the left-hand carriageway changed sides"
    assert 'rule="RHT"' in written, "an explicit RHT was normalized away"
    assert 'rule="left"' in written, "an unknown spelling was deleted rather than kept"

    reloaded, _ = rm.parse_xodr(written)
    again = rm.write_xodr(reloaded, "left_hand_traffic")
    assert written == again, "write -> parse -> write is not a fixed point"
    print("\nRound trip preserved every @rule spelling, and is a fixed point.")


if __name__ == "__main__":
    main()
