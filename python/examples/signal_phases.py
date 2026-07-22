"""The signal PHASE cycle of a junction through the Python bindings (p4-s8, #229).

OpenDRIVE §14.6 gives a junction its static signal wiring — `<controller>`s and
the synchronization group — but DELIBERATELY excludes the cycle itself:
"dynamic content like the signal cycle itself is specified outside of this
standard, for example, in ASAM OpenSCENARIO". So RoadMaker keeps phase timing as
a Layer-1 record (`<userData code="rm:phases">`, ADR-0008), and the schema
mirrors OpenSCENARIO 1.4.0 `Phase{duration,name,trafficSignalStates}` so the P8
export to traffic-signal actions is near-mechanical.

Three things are worth knowing before reading the code.

* The cycle is DERIVED until you edit it. A two-phase four-way gets, per axis, a
  20 s green plus a 3 s yellow CLEARANCE phase — yellow is an EXPLICIT state, not
  an automatic transition. `junction_phases()` reports the derived and the
  authored cycle identically; only `plan.authored` tells them apart.
* The FIRST edit MATERIALIZES that derived cycle (sparsely) into the junction —
  `plan.authored` flips true and `Junction.phases` becomes non-empty — while the
  shape is preserved, so an edit never silently changes a phase you did not touch.
* Scrubbing needs no time-parameterized kernel call: state is piecewise-constant,
  so `plan.phases[phase_index_at(plan, t)]` is the whole answer.

Run:  python signal_phases.py [out.xodr]
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

import roadmaker as rm

NAME = "signal_phases"

STATE_GLYPH = {
    rm.SignalState.RED: "R",
    rm.SignalState.YELLOW: "Y",
    rm.SignalState.GREEN: "G",
    rm.SignalState.OFF: "O",
}


def print_plan(plan: rm.JunctionPhasePlan, label: str) -> None:
    kind = "authored" if plan.authored else "derived"
    print(f"\n{label}  [{kind} cycle, {plan.cycle_duration:.0f} s over {len(plan.phases)} phases]")
    print(f"  controller rows: {plan.controller_odr_ids or ['(none)']}")
    for i, phase in enumerate(plan.phases):
        cells = "  ".join(
            f"{cell.controller_odr_id}={STATE_GLYPH[cell.state]}" for cell in phase.states
        )
        print(
            f"  phase {i}: {phase.name:<12} start {phase.start:5.1f} s  "
            f"dur {phase.duration:4.1f} s  [{cells}]  "
            f"{len(phase.signal_states)} head(s), {len(phase.moving)} moving road(s)"
        )
    if plan.dormant_controller_odr_ids:
        print(f"  dormant controllers (pruned on write): {plan.dormant_controller_odr_ids}")


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "signal_phases.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # --- 1. a roomy four-arm crossing, signalized two-phase -----------------
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]

    options = rm.edit.SignalizeOptions()
    options.tmpl = rm.edit.SignalizeTemplate.TWO_PHASE
    stack.push(network, rm.edit.signalize_junction(network, junction, options))

    # --- 2. the DERIVED cycle -----------------------------------------------
    plan = rm.junction_phases(network, junction)
    print_plan(plan, "just signalized — nothing authored yet")
    assert not plan.authored
    assert not network.junction(junction).phases  # empty ⇔ derived
    assert len(plan.phases) == 4  # green+yellow per axis
    assert plan.cycle_duration == rm.DEFAULT_PHASE_GREEN_SECONDS * 2 + \
        rm.DEFAULT_PHASE_YELLOW_SECONDS * 2  # 46 s
    assert len(plan.controller_odr_ids) == 2  # one row per axis

    # --- 3. author an edit: the first edit MATERIALIZES the cycle -----------
    # Lengthen the first axis-green to 30 s. The other three phases keep their
    # derived durations, materialized verbatim.
    stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))
    authored = rm.junction_phases(network, junction)
    print_plan(authored, "after set_phase_duration(phase 0 -> 30 s)")
    assert authored.authored
    assert network.junction(junction).phases  # now non-empty
    assert authored.phases[0].duration == 30.0
    assert authored.cycle_duration == 56.0  # 30 + 3 + 20 + 3

    # A no-op edit is REFUSED — you cannot re-type a phase's existing value, the
    # command layer's round-trip oracle forbids a command that changes nothing.
    try:
        stack.push(network, rm.edit.set_phase_duration(network, junction, 0, 30.0))
        raise AssertionError("re-typing the same duration should have been refused")
    except ValueError as error:
        print(f"\n  refused (no-op): {error}")

    # --- 4. scrub the cycle with phase_index_at -----------------------------
    print("\nscrubbing the cycle (piecewise-constant, no time call needed):")
    for t in (0.0, 15.0, 32.0, 55.0, 56.0, -1.0):
        index = rm.phase_index_at(authored, t)
        phase = authored.phases[index]
        print(f"  t={t:6.1f} s -> phase {index} '{phase.name}'  ({len(phase.moving)} moving)")

    # --- 5. save -> reload: the authored phases survive ---------------------
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "signal_phases.xodr"
        rm.save_xodr(network, path)
        text = path.read_text()
        assert "rm:phases" in text, "authored phases are written as rm:phases userData"

        reloaded, diagnostics = rm.load_xodr(path)
        assert not diagnostics, [d.message for d in diagnostics]
        reloaded_junction = reloaded.junction(reloaded.junction_ids[0])
        assert reloaded_junction.phases, "authored phases came back"

        reloaded_plan = rm.junction_phases(reloaded, reloaded.junction_ids[0])
        assert reloaded_plan.authored
        assert [p.duration for p in reloaded_plan.phases] == [
            p.duration for p in authored.phases
        ], "durations survived the round trip"

        # save -> reload -> save is byte-identical.
        again = Path(tmp) / "again.xodr"
        rm.save_xodr(reloaded, again)
        assert again.read_bytes() == path.read_bytes(), "round trip is byte-identical"
        print("\n  [round trip ok] authored phases survive save -> reload -> save, byte-identical")

    # --- 6. and it is all one undo away -------------------------------------
    stack.undo(network)  # undo the duration edit
    assert not rm.junction_phases(network, junction).authored
    assert not network.junction(junction).phases
    print("  one undo returned the junction to its derived cycle")

    stack.redo(network)  # ...and one redo re-authors it
    assert rm.junction_phases(network, junction).phases[0].duration == 30.0
    print("  one redo re-authored the 30 s phase")

    assert not rm.validate_network(network), "an authored cycle is a clean network"

    rm.save_xodr(network, out_path)
    print(f"\nwrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
