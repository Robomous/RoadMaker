"""Junction MANEUVERS: turn types, hand-shaped paths, U-turns (#227).

A *maneuver* is ONE connecting road's path through a junction. `junction_maneuvers`
is the single solve every consumer reads — the editor's Maneuver tool, the
properties rows, the command layer's validate-first checks and these bindings —
so none of them can disagree about what a maneuver is or where it may be dragged.

Its turn type is DERIVED. ASAM OpenDRIVE has no turn-type element: §12.2 Table 56
gives `<connection>` exactly @connectingRoad, @contactPoint, @id and
@incomingRoad, and §12.4/§12.4.2 describe a connecting road purely by its
geometry and lane linkage — the turn a driver perceives is implicit in the plan
view. RoadMaker therefore classifies the movement off the arm-face headings and
stores nothing unless the author overrides it (ADR-0008 Layer 1, persisted as
`<userData code="rm:maneuver">` on the junction).

Six commands author a maneuver, and every one of them is a single undo step:

  set_maneuver_locked     keep this road's geometry through a regeneration
  set_maneuver_turn_type  override the derived label, or clear it with None
  set_maneuver_path       reshape it — THE geometry command; locks implicitly
  reset_maneuver          this one road, back to the derivation
  rebuild_maneuvers       the whole junction, back to the derivation
  add_uturn_maneuver      the one turn the planner never emits

Two rules run through all of them. AUTHORS-NOTHING => ERASE: a record left
locking nothing, overriding nothing and shaping nothing is dropped, so an
edit-and-undo pair writes the original bytes. And a NO-OP IS AN ERROR, judged
against the EFFECTIVE value rather than against the record.

Run:  python junction_maneuvers.py [out.xodr]
"""

from __future__ import annotations

import sys

import roadmaker as rm

NAME = "junction_maneuvers"

LABEL = {
    rm.TurnType.LEFT: "left",
    rm.TurnType.STRAIGHT: "straight",
    rm.TurnType.RIGHT: "right",
    rm.TurnType.UTURN: "u-turn",
}


def describe(network: rm.RoadNetwork, junction: rm.JunctionId, label: str) -> None:
    print(f"\n{label}")
    for info in rm.junction_maneuvers(network, junction):
        flags = "".join(
            (
                "L" if info.locked else "-",
                "O" if info.overridden else "-",
                "U" if info.is_uturn_explicit else "-",
            )
        )
        shape = f"{len(info.control_points)} pts" if info.control_points else "derived"
        print(
            f"  turn {info.road_odr_id:<3} {LABEL[info.effective]:<8} [{flags}] "
            f"lanes {info.from_lane:+d} -> {info.to_lane:+d}  {shape:<9} "
            f"offsets ({info.start_offset:+.2f}, {info.end_offset:+.2f})  "
            f"{len(info.path)} samples"
        )


def check_round_trip(network: rm.RoadNetwork, step: str) -> None:
    """save -> reload -> save must be byte-identical, and reload must be silent."""
    text = rm.write_xodr(network, NAME)
    reloaded, diagnostics = rm.parse_xodr(text)
    assert not diagnostics, f"{step}: {[d.message for d in diagnostics]}"
    assert rm.write_xodr(reloaded, NAME) == text, f"{step}: not byte-identical"
    print(f"  [round trip ok] {step} — {len(text)} bytes, no diagnostics")


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "junction_maneuvers.xodr"

    network = rm.RoadNetwork()
    stack = rm.edit.EditStack()

    # --- 1. a four-arm crossing --------------------------------------------
    params = rm.edit.assembly.IntersectionParams()
    params.gap_m = 24.0
    stack.push(
        network,
        rm.edit.assembly.x_intersection(network, rm.edit.assembly.Pose(0.0, 0.0, 0.0), params),
    )
    junction = network.junction_ids[0]
    describe(network, junction, "generated crossing — every maneuver DERIVED:")
    check_round_trip(network, "generated")
    assert not network.junction(junction).maneuvers, "nothing authored, nothing stored"

    # --- 2. a semantic override --------------------------------------------
    # Purely a label: geometry never moves, which is why an explicit rebuild
    # keeps it while clearing everything else.
    left = next(m for m in rm.junction_maneuvers(network, junction)
                if m.effective == rm.TurnType.LEFT)
    before_override = rm.write_xodr(network, NAME)
    stack.push(
        network, rm.edit.set_maneuver_turn_type(network, junction, left.road, rm.TurnType.UTURN)
    )
    relabelled = next(m for m in rm.junction_maneuvers(network, junction) if m.road == left.road)
    print(
        f"\nturn {relabelled.road_odr_id}: computed {LABEL[relabelled.computed]}, "
        f"overridden to {LABEL[relabelled.effective]} — path unchanged: "
        f"{relabelled.path == left.path}"
    )
    check_round_trip(network, "turn type overridden")

    # AUTHORS-NOTHING => ERASE. Clearing drops the record, not merely the field.
    stack.push(network, rm.edit.set_maneuver_turn_type(network, junction, left.road, None))
    assert not network.junction(junction).maneuvers
    assert rm.write_xodr(network, NAME) == before_override
    print("cleared — the record is gone, and the file matches byte for byte")

    # Setting the COMPUTED value clears the override rather than pinning it, so
    # asking for it when there is no override is refused.
    try:
        stack.push(
            network,
            rm.edit.set_maneuver_turn_type(network, junction, left.road, left.computed),
        )
        raise AssertionError("a no-op turn type should have been refused")
    except ValueError as error:
        print(f"  refused (no-op): {error}")

    # --- 3. reshape a turn --------------------------------------------------
    # The path is refitted as a G1 clothoid chain through
    # [start anchor + start_offset, control points..., end anchor + end_offset]
    # with the END HEADINGS LOCKED to the arm faces, so a hand-shaped maneuver
    # still meets its arms tangentially (§12.4.2); the road's length, elevation
    # and blended lane width are rewritten in the same step.
    mid = left.path[len(left.path) // 2]
    print(
        f"\nturn {left.road_odr_id} start slide: "
        f"{left.start_slide.min_offset:+.2f} .. {left.start_slide.max_offset:+.2f} m "
        f"along the incoming arm's face"
    )
    stack.push(
        network,
        rm.edit.set_maneuver_path(
            network,
            junction,
            left.road,
            [(mid[0] + 1.5, mid[1] + 1.5)],
            start_offset=0.8,
        ),
    )
    shaped = next(m for m in rm.junction_maneuvers(network, junction) if m.road == left.road)
    print(
        f"reshaped: locked={shaped.locked} (implicit — hand-shaped geometry is "
        f"one undo away, not two), {len(shaped.control_points)} interior point, "
        f"start_offset={shaped.start_offset:+.2f}"
    )
    check_round_trip(network, "path reshaped")

    # --- 4. the lock earns its keep ----------------------------------------
    shaped_path = shaped.path
    stack.push(network, rm.edit.regenerate_junction(network, junction))
    kept = next(m for m in rm.junction_maneuvers(network, junction) if m.road == left.road)
    assert kept.locked and kept.path == shaped_path
    print("\nafter regenerate_junction the locked maneuver kept its exact path")

    # --- 5. the turn the planner never emits --------------------------------
    # A U-turn is a policy decision, not a derivable movement, so the generator
    # skips the same-arm pair. This creates the connecting road, the connection
    # and a LOCKED record together — the lock is what keeps the next
    # regeneration from dropping a turn no plan contains.
    arm = network.junction(junction).arms[0]
    stack.push(network, rm.edit.add_uturn_maneuver(network, junction, arm))
    uturn = next(m for m in rm.junction_maneuvers(network, junction) if m.is_uturn_explicit)
    print(
        f"\nadded an explicit U-turn on arm {network.road(arm.road).odr_id}: "
        f"turn {uturn.road_odr_id}, locked={uturn.locked}, from == to: "
        f"{uturn.from_ == uturn.to}"
    )
    describe(network, junction, "with the U-turn and the hand-shaped left:")
    check_round_trip(network, "u-turn added")

    stack.push(network, rm.edit.regenerate_junction(network, junction))
    assert any(m.is_uturn_explicit for m in rm.junction_maneuvers(network, junction))
    print("the U-turn survived a regeneration too — that is what the lock is for")

    # It has no derived geometry to fall back on, so it cannot be reset.
    try:
        stack.push(network, rm.edit.reset_maneuver(network, junction, uturn.road))
        raise AssertionError("resetting an explicit U-turn should have been refused")
    except ValueError as error:
        print(f"  refused (reset a U-turn): {error}")

    # --- 6. back to the derivation ------------------------------------------
    # A semantic override to prove what a rebuild keeps.
    straight = next(m for m in rm.junction_maneuvers(network, junction)
                    if m.effective == rm.TurnType.STRAIGHT)
    stack.push(
        network,
        rm.edit.set_maneuver_turn_type(network, junction, straight.road, rm.TurnType.RIGHT),
    )

    stack.push(network, rm.edit.rebuild_maneuvers(network, junction))
    describe(network, junction, "after rebuild_maneuvers — geometry cleared, labels kept:")
    rebuilt = next(m for m in rm.junction_maneuvers(network, junction) if m.road == left.road)
    assert not rebuilt.locked and not rebuilt.control_points
    assert not any(m.is_uturn_explicit for m in rm.junction_maneuvers(network, junction))
    label_kept = next(m for m in rm.junction_maneuvers(network, junction)
                      if m.road == straight.road)
    assert label_kept.overridden and label_kept.effective == rm.TurnType.RIGHT
    print(
        "hand-shaped geometry replaced, the explicit U-turn dropped, "
        f"the turn-type override on {label_kept.road_odr_id} SURVIVED "
        "(semantic, not geometric)"
    )
    check_round_trip(network, "rebuilt")

    # --- 7. save ------------------------------------------------------------
    findings = rm.validate_network(network)
    print(f"\nvalidate_network: {len(findings)} findings")
    for finding in findings:
        print(f"  {finding.severity} {finding.location}: {finding.message}")
    assert not findings

    rm.save_xodr(network, out_path, NAME)
    records = len(network.junction(junction).maneuvers)
    print(f"wrote {out_path} — {records} rm:maneuver record(s), {len(network.road_ids)} roads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
