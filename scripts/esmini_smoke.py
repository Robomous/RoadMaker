#!/usr/bin/env python3
"""esmini round-trip smoke check for exported OpenDRIVE (issue #51).

The cross-cutting quality gate owned by M3a (docs/roadmap/roadmap.md,
docs/design/m3a/05_editor_and_docs.md section 4): every golden .xodr must
load headless in esmini without errors. esmini is an EXTERNAL smoke tool —
a pinned release binary fetched in CI like a test fixture, never linked
into any RoadMaker target and never redistributed (MPL-2.0, verified
against docs/standards/dependencies.md 2026-07-11).

For each .xodr the script generates a minimal OpenSCENARIO wrapper (esmini
has no bare road-network mode — the scenario is the entry point), runs
`esmini --headless` for half a simulated second, and fails on a non-zero
exit or a load-error marker in the log. `--expect-fail` inverts the check
for the deliberately-broken fixture that guards the gate itself.

Usage:
    esmini_smoke.py --esmini <esmini-binary> [--expect-fail] <xodr> [...]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# Minimal but complete OpenSCENARIO 1.2 wrapper: one ego teleported to a
# world position (no road references — the wrapper must work for ANY road
# network) and a plain half-second stop trigger. esmini still parses and
# builds the full OpenDRIVE network at init, which is exactly the smoke
# we want.
XOSC_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<OpenSCENARIO>
  <FileHeader revMajor="1" revMinor="2" date="2026-01-01T00:00:00"
              description="RoadMaker esmini smoke wrapper" author="RoadMaker CI"/>
  <ParameterDeclarations/>
  <CatalogLocations/>
  <RoadNetwork>
    <LogicFile filepath="{xodr}"/>
  </RoadNetwork>
  <Entities>
    <ScenarioObject name="Ego">
      <Vehicle name="car" vehicleCategory="car">
        <ParameterDeclarations/>
        <Performance maxSpeed="70" maxAcceleration="5" maxDeceleration="10"/>
        <BoundingBox>
          <Center x="1.4" y="0.0" z="0.75"/>
          <Dimensions width="2.0" length="5.0" height="1.5"/>
        </BoundingBox>
        <Axles>
          <FrontAxle maxSteering="0.5" wheelDiameter="0.6" trackWidth="1.8"
                     positionX="2.98" positionZ="0.3"/>
          <RearAxle maxSteering="0.0" wheelDiameter="0.6" trackWidth="1.8"
                    positionX="0.0" positionZ="0.3"/>
        </Axles>
        <Properties/>
      </Vehicle>
    </ScenarioObject>
  </Entities>
  <Storyboard>
    <Init>
      <Actions>
        <Private entityRef="Ego">
          <PrivateAction>
            <TeleportAction>
              <Position>
                <WorldPosition x="0.0" y="0.0" z="0.0" h="0.0"/>
              </Position>
            </TeleportAction>
          </PrivateAction>
        </Private>
      </Actions>
    </Init>
    <StopTrigger>
      <ConditionGroup>
        <Condition name="end" delay="0.0" conditionEdge="rising">
          <ByValueCondition>
            <SimulationTimeCondition value="0.5" rule="greaterThan"/>
          </ByValueCondition>
        </Condition>
      </ConditionGroup>
    </StopTrigger>
  </Storyboard>
</OpenSCENARIO>
"""

# esmini exits 0 for some recoverable problems it merely logs; any of these
# markers in the output means the road network did NOT load cleanly.
ERROR_MARKERS = (
    "Failed to load OpenDRIVE",
    "Failed to parse OpenDRIVE",
    "Failed to load road network",
    "Invalid OpenDRIVE",
    "[error]",
)


def smoke_one(esmini: Path, xodr: Path) -> tuple[bool, str]:
    """Returns (loaded_cleanly, combined_output)."""
    with tempfile.TemporaryDirectory(prefix="esmini_smoke_") as tmp:
        wrapper = Path(tmp) / f"{xodr.stem}_smoke.xosc"
        wrapper.write_text(XOSC_TEMPLATE.format(xodr=xodr.resolve().as_posix()),
                           encoding="utf-8")
        result = subprocess.run(
            [str(esmini), "--headless", "--osc", str(wrapper),
             "--fixed_timestep", "0.05", "--disable_log"],
            capture_output=True,
            text=True,
            timeout=120,
        )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        return False, output
    lowered = output.lower()
    if any(marker.lower() in lowered for marker in ERROR_MARKERS):
        return False, output
    return True, output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esmini", required=True, type=Path,
                        help="path to the esmini binary")
    parser.add_argument("--expect-fail", action="store_true",
                        help="invert the check (broken-fixture guard)")
    parser.add_argument("xodr", nargs="+", type=Path)
    args = parser.parse_args()

    failures = 0
    for xodr in args.xodr:
        if not xodr.is_file():
            print(f"FAIL {xodr}: file not found")
            failures += 1
            continue
        ok, output = smoke_one(args.esmini, xodr)
        if args.expect_fail:
            if ok:
                print(f"FAIL {xodr}: loaded cleanly but was expected to fail "
                      "(the broken-fixture guard no longer guards)")
                failures += 1
            else:
                print(f"OK   {xodr}: rejected as expected")
        elif ok:
            print(f"OK   {xodr}: loads cleanly in esmini")
        else:
            print(f"FAIL {xodr}: esmini could not load it\n--- esmini output ---")
            print(output)
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
