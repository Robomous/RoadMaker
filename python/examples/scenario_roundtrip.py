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

"""Reading a .xosc back: rm.osc.parse_xosc / load_xosc (p8-s1, #245).

The companion to scenario_write.py. Three things are worth knowing before
reading the code.

* THE ROUND-TRIP CLAIM IS NARROWER THAN "BYTE-STABLE", and the narrow one is
  what holds. The writer emits a canonical attribute order and an
  always-present skeleton, so a FOREIGN file re-canonicalizes the first time
  it is written. What is guaranteed is idempotence from the written form:
  `write(S) == write(parse(write(S)))`, byte for byte, at every revision.
* NOTHING IS EVER SILENTLY DROPPED. Every element this version does not model —
  a `<MiscObject>`, a `<LanePosition>` teleport, a whole `<Story>` — comes back
  out verbatim on the preserved tier, with a diagnostic saying so. That is what
  makes it safe to open someone else's scenario, change one phase duration and
  save.
* A PARSE ONLY FAILS ON A STRUCTURAL PROBLEM. Malformed XML, no `<OpenSCENARIO>`
  root, or a catalog document. A bad number is a diagnostic, not a lost file.

Run:  python scenario_roundtrip.py
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import roadmaker as rm

# A scenario carrying three things this version does not model: a MiscObject
# entity, a teleport onto a lane position rather than a world position, and a
# whole Story. All three must survive.
FOREIGN = """<?xml version="1.0" encoding="UTF-8"?>
<OpenSCENARIO>
  <FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="authored elsewhere" author="SomeOtherTool" vendorTag="7"/>
  <CatalogLocations/>
  <RoadNetwork><LogicFile filepath="town.xodr"/></RoadNetwork>
  <Entities>
    <ScenarioObject name="Cone"><MiscObject name="cone" mass="5" miscObjectCategory="obstacle">
      <BoundingBox><Center x="0" y="0" z="0.3"/><Dimensions width="0.3" length="0.3" height="0.6"/></BoundingBox>
    </MiscObject></ScenarioObject>
  </Entities>
  <Storyboard>
    <Init><Actions><Private entityRef="Cone"><PrivateAction><TeleportAction>
      <Position><LanePosition roadId="1" laneId="-1" s="12.5" offset="0"/></Position>
    </TeleportAction></PrivateAction></Private></Actions></Init>
    <Story name="overtake"><Act name="act1"/></Story>
    <StopTrigger><ConditionGroup><Condition name="end" delay="0" conditionEdge="rising">
      <ByValueCondition><SimulationTimeCondition value="10" rule="greaterThan"/></ByValueCondition>
    </Condition></ConditionGroup></StopTrigger>
  </Storyboard>
</OpenSCENARIO>
"""


def main() -> int:
    # --- 1. read a scenario this version cannot fully model -----------------
    result = rm.osc.parse_xosc(FOREIGN, "foreign.xosc")
    print(f"parsed as OpenSCENARIO {result.rev_major}.{result.rev_minor}, "
          f"{len(result.diagnostics)} diagnostic(s):")
    for d in result.diagnostics:
        rule = f"  [{d.rule_id}]" if d.rule_id else ""
        print(f"  - {d.location}: {d.message}{rule}")
    assert result.diagnostics, "unmodeled content must be reported, not absorbed in silence"

    scenario = result.scenario

    # The entity object is the variant's monostate arm: a MiscObject is one of
    # the five EntityObject choices and this version models two of them.
    cone = scenario.entities.scenario_objects[0]
    assert cone.name == "Cone"
    assert cone.entity_object is None, "an unmodeled entity object reads as None"
    assert cone.preserved.children, "...and rides the preserved tier"

    # The teleport is NOT modeled, because its position is a lane position and
    # TeleportAction holds a world position. Modeling it anyway would silently
    # move the cone to the origin.
    action = scenario.storyboard.init.actions.privates[0].actions[0]
    assert action.teleport is None, "a lane-positioned teleport must not be half-modeled"

    assert len(scenario.storyboard.preserved_stories) == 1, "the Story is kept whole"

    # --- 2. everything comes back out ---------------------------------------
    text = rm.osc.write_xosc(scenario)
    for survivor in ('<MiscObject', '<LanePosition', '<Story name="overtake"', 'vendorTag="7"'):
        assert survivor in text, f"{survivor} was dropped"
    print("\n  [never drops] MiscObject, LanePosition, Story and an unknown attribute all survived")

    # --- 3. the round trip is a FIXED POINT, not a byte-for-byte echo -------
    # The foreign document re-canonicalizes on this first write; from here on,
    # writing is idempotent. That is the property GW-6 fingerprints state with.
    again = rm.osc.write_xosc(rm.osc.parse_xosc(text).scenario)
    assert again == text, "write -> parse -> write must be byte-identical"
    print("  [idempotent]  write -> parse -> write is byte-identical")

    at_1_4 = rm.osc.write_xosc(scenario, rm.osc.OscVersion.V1_4)
    assert rm.osc.write_xosc(rm.osc.parse_xosc(at_1_4).scenario, rm.osc.OscVersion.V1_4) == at_1_4
    print("  [idempotent]  ...and at revision 1.4 too")

    # --- 4. load_xosc adds the findings only a PATH can support -------------
    with tempfile.TemporaryDirectory() as tmp:
        # Deliberately the wrong extension, and the network is not on disk.
        path = Path(tmp) / "scene.xml"
        path.write_text(text, encoding="utf-8")
        loaded = rm.osc.load_xosc(path)
        cited = {d.rule_id for d in loaded.diagnostics}
        assert rm.osc.RULE_FILE_ENDING in cited, "a non-.xosc extension is an advisory"
        assert rm.osc.RULE_ROAD_NETWORK_AVAILABILITY in cited, "town.xodr is not beside it"
        print(f"\n  [load_xosc]   cited {rm.osc.RULE_FILE_ENDING}")
        print(f"  [load_xosc]   cited {rm.osc.RULE_ROAD_NETWORK_AVAILABILITY}")

        # Put the network beside it under the right name, and both go away.
        good = Path(tmp) / "scene.xosc"
        good.write_text(text, encoding="utf-8")
        (Path(tmp) / "town.xodr").write_text("<OpenDRIVE/>", encoding="utf-8")
        assert not [d for d in rm.osc.load_xosc(good).diagnostics
                    if d.rule_id in (rm.osc.RULE_FILE_ENDING,
                                     rm.osc.RULE_ROAD_NETWORK_AVAILABILITY)]
        print("  [load_xosc]   both clear once the file is named and placed correctly")

    # --- 5. a structural problem raises; a bad number does not --------------
    try:
        rm.osc.parse_xosc("<OpenSCENARIO><Entities>")
        raise AssertionError("malformed XML should have raised")
    except ValueError as error:
        print(f"\n  refused (malformed): {error}")

    lenient = rm.osc.parse_xosc(
        FOREIGN.replace('value="10"', 'value="soon"'), "foreign.xosc")
    assert any("soon" in d.message for d in lenient.diagnostics), \
        "the unparseable spelling must survive in the diagnostic"
    print("  kept (bad number): the value's spelling is reported, the file still loads")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
