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

"""esmini smoke check for exported OpenDRIVE and OpenSCENARIO (issues #51, #245).

The cross-cutting quality gate owned by M3a (docs/roadmap/roadmap.md,
docs/design/m3a/05_editor_and_docs.md section 4): every golden .xodr must
load headless in esmini without errors. esmini is an EXTERNAL smoke tool —
a pinned release binary fetched in CI like a test fixture, never linked
into any RoadMaker target and never redistributed (MPL-2.0, verified
against docs/standards/dependencies.md 2026-07-11).

TWO MODES.

* Default: each argument is a .xodr, and the script builds a minimal wrapper
  scenario for it (esmini has no bare road-network mode — the scenario is the
  entry point). ★ THAT WRAPPER IS WRITTEN BY THE KERNEL since p8-s5 (#249):
  `rm.osc.write_xosc`, authored through the same `rm.osc.edit` commands the
  editor pushes. It used to be a hardcoded string in this file, which proved
  that a human can write valid OpenSCENARIO and nothing about what RoadMaker
  emits. The mode therefore needs the Python bindings installed
  (`pip install ./python`) and fails loudly without them rather than falling
  back to a literal nobody would notice.
* `--xosc`: each argument is a real, tracked .xosc, fed to esmini as-is.
  Added by p8-s1 (#245), when RoadMaker first emitted scenarios of its own.
  Needs no bindings — there is no wrapper to build.

Either way it runs `esmini --headless` for half a simulated second and fails
on a non-zero exit or a load-error marker in the log. `--expect-fail` inverts
the check for the deliberately-broken fixtures that guard the gate itself.

★ WHAT THIS GATE DOES AND DOES NOT CATCH, measured against v3.5.0 on
2026-07-30 and 2026-08-01 rather than assumed. It REJECTS (exit 255): truncated XML, a
duplicated element, a <LogicFile> that resolves to nothing, and a dangling
entityRef. It ACCEPTS IN SILENCE, with a byte-identical log: a
trafficSignalId naming no <signal>, a garbage @state token, a
trafficSignalControllerRef naming no controller, a nonexistent phase name,
and an undefined revMinor. So for the traffic-signal half the checker-rule
UIDs in core/include/roadmaker/osc/rules.hpp are not "additive to esmini" —
they are the only check that exists. Do not read a green run here as a
statement about signal content.

THE OTHER HALF OF THAT SENTENCE NOW HAS AN OWNER (#533):
`osc::validate_scenario_against_network` resolves every signal id, controller
id and lane anchor against the .xodr, which is exactly what this gate cannot
do. A green run here plus an empty finding list there is the whole check;
neither alone is.

Usage:
    esmini_smoke.py --esmini <esmini-binary> [--expect-fail] <xodr> [...]
    esmini_smoke.py --esmini <esmini-binary> --xosc [--expect-fail] <xosc> [...]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# ★ THE WRAPPER IS WRITTEN BY THE KERNEL, NOT BY THIS FILE (p8-s5, #249).
#
# It used to be a hardcoded OpenSCENARIO 1.2 string living here — which meant
# the gate proved that a HUMAN can write valid OpenSCENARIO, a fact never in
# doubt, and said nothing about what RoadMaker emits. Every wrapper is now
# `rm.osc.write_xosc` output, authored through the same `rm.osc.edit` commands
# the editor pushes, so the file esmini parses is a file the product wrote.
#
# NO FALLBACK TO A LITERAL. A silent fallback would let CI keep passing on the
# old path with nobody looking — the #506 failure mode exactly (a cache key that
# skipped a fetch and tested the binary the bump was meant to replace). If
# `roadmaker` is not importable, the wrapper mode fails loudly and says how to
# install it. The `--xosc` mode needs no wrapper and therefore no import.
_IMPORT_ERROR: str | None = None
try:
    import roadmaker as rm
except ImportError as exc:  # pragma: no cover - exercised only by a broken env
    rm = None
    _IMPORT_ERROR = str(exc)


def build_wrapper(xodr: Path) -> str:
    """A minimal scenario for `xodr`, written by the kernel.

    One ego at the world origin (no road references — the wrapper must work for
    ANY road network) and a half-second stop trigger, so the gate measures the
    OpenDRIVE build rather than waiting out a simulation.
    """
    if rm is None:
        raise SystemExit(
            "esmini_smoke.py needs the roadmaker Python bindings to build its wrapper "
            f"scenario (`pip install ./python`): {_IMPORT_ERROR}"
        )

    scenario = rm.osc.Scenario()
    stack = rm.osc.edit.ScenarioStack()

    header = scenario.header
    header.description = "RoadMaker esmini smoke wrapper"
    scenario.header = header

    stack.push(scenario, rm.osc.edit.set_logic_file(scenario, xodr.resolve().as_posix()))
    stack.push(
        scenario,
        rm.osc.edit.place_scenario_object(
            scenario, rm.osc.make_actor(rm.osc.ActorKind.Car, "Ego"), rm.osc.WorldPosition()
        ),
    )

    end = rm.osc.Condition()
    end.name = "end"
    timing = rm.osc.SimulationTimeCondition()
    timing.value = 0.5
    timing.rule = "greaterThan"
    end.simulation_time = timing
    group = rm.osc.ConditionGroup()
    group.conditions = [end]
    stop = rm.osc.Trigger()
    stop.condition_groups = [group]
    stack.push(scenario, rm.osc.edit.set_stop_trigger(scenario, stop))

    # A wrapper the kernel itself refuses would make every failure below
    # ambiguous — is the .xodr bad, or the wrapper? Say which, here.
    findings = rm.osc.validate_scenario(scenario)
    blocking = [f for f in findings if f.severity == rm.Severity.ERROR]
    if blocking:
        raise SystemExit(
            "the generated wrapper scenario does not validate: "
            + "; ".join(f.message for f in blocking)
        )
    return rm.osc.write_xosc(scenario)


# esmini exits 0 for some recoverable problems it merely logs; any of these
# markers in the output means the document did NOT load cleanly.
#
# ★ THE FIRST FOUR ARE ALL OPENDRIVE-WORDED, which the P8 discovery flagged
# (#505 §2) as a hole: a scenario-level failure would have slipped through them.
# It does not, and the reason is the LAST two rather than the first four —
# `[error]` is generic, and the return code is checked before any marker. That
# was measured rather than assumed across p8-s2, p8-s3 and p8-s4: a dangling
# entityRef, a dangling lane anchor, an invalid `Event/@priority` and an invalid
# `@dynamicsShape` are each caught by one or the other. The scenario-worded
# entries below are kept anyway, because a marker that never fires costs
# nothing and a missing one costs a false green.
ERROR_MARKERS = (
    "Failed to load OpenDRIVE",
    "Failed to parse OpenDRIVE",
    "Failed to load road network",
    "Invalid OpenDRIVE",
    "Failed to load OpenSCENARIO",
    "Failed to parse OpenSCENARIO",
    "[error]",
    "Exception",
)


def run_esmini(esmini: Path, scenario: Path) -> tuple[bool, str]:
    """Runs one scenario. Returns (loaded_cleanly, combined_output)."""
    result = subprocess.run(
        [str(esmini), "--headless", "--osc", str(scenario),
         "--fixed_timestep", "0.05", "--disable_log"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    output = result.stdout + result.stderr
    # The return code is checked FIRST and on its own: esmini exits 255 on a
    # truncated document while printing none of the markers below, so a
    # marker-only check would pass the most broken input there is.
    if result.returncode != 0:
        return False, output
    lowered = output.lower()
    if any(marker.lower() in lowered for marker in ERROR_MARKERS):
        return False, output
    return True, output


def smoke_one(esmini: Path, xodr: Path) -> tuple[bool, str]:
    """Smokes a .xodr through a wrapper scenario the KERNEL writes."""
    with tempfile.TemporaryDirectory(prefix="esmini_smoke_") as tmp:
        wrapper = Path(tmp) / f"{xodr.stem}_smoke.xosc"
        wrapper.write_text(build_wrapper(xodr), encoding="utf-8")
        return run_esmini(esmini, wrapper)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esmini", required=True, type=Path,
                        help="path to the esmini binary")
    parser.add_argument("--expect-fail", action="store_true",
                        help="invert the check (broken-fixture guard)")
    parser.add_argument("--xosc", action="store_true",
                        help="the arguments are real .xosc scenarios, not .xodr "
                             "networks to wrap")
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()

    failures = 0
    for path in args.paths:
        if not path.is_file():
            print(f"FAIL {path}: file not found")
            failures += 1
            continue
        ok, output = (run_esmini(args.esmini, path) if args.xosc
                      else smoke_one(args.esmini, path))
        if args.expect_fail:
            if ok:
                print(f"FAIL {path}: loaded cleanly but was expected to fail "
                      "(the broken-fixture guard no longer guards)")
                failures += 1
            else:
                print(f"OK   {path}: rejected as expected")
        elif ok:
            print(f"OK   {path}: loads cleanly in esmini")
        else:
            print(f"FAIL {path}: esmini could not load it\n--- esmini output ---")
            print(output)
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
