"""Auto-SIGNALIZE a junction: heads, signs, controllers, sync groups (#228).

Until this sprint RoadMaker had no model for OpenDRIVE's signal controllers at
all. A top-level `<controller>` in an input file was warned about and DROPPED,
and a `<junction><controller>` was dropped without even a warning. Both now
load, round-trip and can be authored, which is what makes a signalized junction
exportable rather than merely drawable.

Three things are worth knowing before reading the code.

* A `<controller>` (§14.6, Table 128) is TOP-LEVEL — a child of `<OpenDRIVE>`,
  not of a road or a junction — and its `<control>` children (Table 129) name
  their signals by STRING `@signalId`. RoadMaker stores exactly that, so a
  dangling reference in third-party input survives a round trip and is reported
  by `validate_network` rather than silently repaired. A junction only
  REFERENCES a controller, through its synchronization group (§12.14, Table 84).
* Which movements a head gates is DERIVED, never stored: the maneuvers leaving
  that arm. `junction_signals()` is the single solve every consumer reads.
* Two of the four templates are STATIC. An all-way stop has no phases, so it
  creates no controllers at all — not an empty one, none.

Run:  python signalize_junction.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm

NAME = "signalize_junction"

TEMPLATES = {
    rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT: "protected left",
    rm.edit.SignalizeTemplate.TWO_PHASE: "two phase",
    rm.edit.SignalizeTemplate.ALL_WAY_STOP: "all-way stop",
    rm.edit.SignalizeTemplate.TWO_WAY_STOP: "two-way stop",
}


def describe(network: rm.RoadNetwork, junction: rm.JunctionId, label: str) -> None:
    print(f"\n{label}")
    for approach in rm.junction_signals(network, junction):
        arm = network.road(approach.arm.road)
        heads = [network.signal(signal_id).odr_id for signal_id in approach.signal_ids]
        groups = approach.controller_odr_ids or ["-"]
        print(
            f"  arm {arm.odr_id:<3} heading {approach.heading:+.2f} rad  "
            f"stop s={approach.s_stop:6.2f}  gates {len(approach.gated)} movement(s)  "
            f"{'lights' if approach.dynamic else 'signs ':<6} "
            f"heads [{', '.join(heads) or '-'}]  groups [{', '.join(groups)}]"
        )
    record = network.junction(junction).signalization
    print(
        f"  applied template: {record.tmpl or '(none)'}"
        f"{f', mounted on {record.mount_model}' if record.mount_model else ''}  "
        f"| {network.controller_count} controller(s), "
        f"{len(network.junction(junction).junction_controllers)} in the sync group"
    )


def check_round_trip(network: rm.RoadNetwork, step: str) -> None:
    """save -> reload -> save must be byte-identical, and reload must be silent."""
    text = rm.write_xodr(network, NAME)
    reloaded, diagnostics = rm.parse_xodr(text)
    assert not diagnostics, f"{step}: {[d.message for d in diagnostics]}"
    assert rm.write_xodr(reloaded, NAME) == text, f"{step}: not byte-identical"
    print(f"  [round trip ok] {step} — {len(text)} bytes, no diagnostics")


def options(tmpl: rm.edit.SignalizeTemplate, mount_model: str = "") -> rm.edit.SignalizeOptions:
    value = rm.edit.SignalizeOptions()
    value.tmpl = tmpl
    value.mount_model = mount_model
    return value


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "signalize_junction.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # --- 1. a roomy four-arm crossing ---------------------------------------
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    params.arm_length_m = 80.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]
    describe(network, junction, "unsignalized — every approach reads, nothing controls it:")
    check_round_trip(network, "unsignalized")
    pristine = rm.write_xodr(network, NAME)
    assert "<controller" not in pristine, "an unsignalized file emits no controller at all"

    # --- 2. the dynamic templates -------------------------------------------
    # FOUR_WAY_PROTECTED_LEFT puts a through head and a protected-left head on
    # every approach that has a left turn, and groups them per AXIS: one
    # controller for the axis's through heads, one for its protected lefts.
    stack.push(
        network,
        rm.edit.signalize_junction(
            network, junction, options(rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT)
        ),
    )
    describe(network, junction, "protected-left signalization:")
    for controller_id in network.controller_ids:
        controller = network.controller(controller_id)
        driven = ", ".join(control.signal_odr_id for control in controller.controls)
        print(f"  <controller id={controller.odr_id}> drives signals [{driven}]")
    check_round_trip(network, "protected left")

    # Signalizing REPLACES: switching templates never leaves two generations of
    # heads behind, so this is 4 permissive heads, not 12.
    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    describe(network, junction, "re-templated to two-phase (permissive lefts):")
    assert network.signal_count == 4 and network.controller_count == 2
    check_round_trip(network, "two phase")

    # Re-applying the IDENTICAL template would change nothing, and the command
    # layer's round-trip oracle forbids a no-op command.
    try:
        stack.push(
            network,
            rm.edit.signalize_junction(
                network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)
            ),
        )
        raise AssertionError("re-applying the same template should have been refused")
    except ValueError as error:
        print(f"\n  refused (no-op): {error}")

    # --- 3. the static templates --------------------------------------------
    # GW-4 step 3: a stop-controlled junction has no phases, so NO phase data is
    # created — this is what keeps the engine from assuming every signalized
    # junction has traffic lights.
    for tmpl in (rm.edit.SignalizeTemplate.ALL_WAY_STOP, rm.edit.SignalizeTemplate.TWO_WAY_STOP):
        stack.push(network, rm.edit.signalize_junction(network, junction, options(tmpl)))
        describe(network, junction, f"{TEMPLATES[tmpl]} — static signs:")
        assert network.controller_count == 0, "a static template creates no controllers"
        assert not network.junction(junction).junction_controllers
        check_round_trip(network, TEMPLATES[tmpl])

    # --- 4. mounts: the #323 assemblies extension point ----------------------
    # Each head also gets a physical prop, and the pairing is recorded as a LIST
    # of object ids per signal — so when assemblies land, an assembly's parts
    # drop straight in with no schema change.
    stack.push(
        network,
        rm.edit.signalize_junction(
            network,
            junction,
            options(rm.edit.SignalizeTemplate.FOUR_WAY_PROTECTED_LEFT, "streetlight_single"),
        ),
    )
    print("\nsignal -> prop mounts:")
    for mount in network.junction(junction).signal_mounts:
        print(f"  signal {mount.signal_odr_id:<3} -> objects {mount.object_odr_ids}")
    check_round_trip(network, "mounted")

    # --- 5. a dangling <control> is a finding, not a repair ------------------
    # Erasing a signal deliberately does NOT cascade into controllers — that is
    # what keeps delete_signal a leaf op — so the reference stays, is written
    # out, and is reported.
    victim = network.signal_ids[0]
    driven_id = network.signal(victim).odr_id
    network.erase_signal(victim)
    for finding in rm.validate_network(network):
        rule = finding.rule_id or "(no ASAM rule id — RoadMaker advisory)"
        print(f"\n  validate: {finding.message}\n            {rule}")
    assert any(driven_id in d.message for d in rm.validate_network(network))
    check_round_trip(network, "dangling control")

    # --- 6. the exact inverse ------------------------------------------------
    stack.push(
        network,
        rm.edit.signalize_junction(network, junction, options(rm.edit.SignalizeTemplate.TWO_PHASE)),
    )
    stack.push(network, rm.edit.clear_signalization(network, junction))
    describe(network, junction, "cleared:")
    assert network.signal_count == 0 and network.controller_count == 0
    assert rm.write_xodr(network, NAME) == pristine, "clearing writes the original bytes"
    print("\ncleared file is byte-identical to the unsignalized one")

    # ...and every step is ONE undo away.
    stack.undo(network)
    assert network.controller_count == 2
    print("one undo brought the whole two-phase signalization back")

    rm.save_xodr(network, out_path)
    print(f"\nwrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
