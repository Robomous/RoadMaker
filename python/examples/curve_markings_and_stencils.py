#!/usr/bin/env python3
# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Free-form marking curves + arrow stencils (p3-s4): author and export.

Builds a straight road, draws one free-form marking curve down it
(edit.apply_marking_curve_asset materializes an (s,t) centreline into an
object's band outline, <markings>, and rm:markingCurve userData), places the
six arrow stencils along a lane (edit.apply_stencil_asset authors one closed
cornerLocal glyph + rm:stencil userData), then writes OpenDRIVE.

Usage:
    python curve_markings_and_stencils.py output.xodr
"""

import sys

import roadmaker as rm

ARROWS = [
    "arrowStraight",
    "arrowLeft",
    "arrowRight",
    "arrowLeftRight",
    "arrowStraightLeft",
    "arrowStraightRight",
]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    xodr_path = sys.argv[1]

    network = rm.RoadNetwork()
    road = rm.author_clothoid_road(
        network, [(0.0, 0.0), (120.0, 0.0)], rm.LaneProfile.two_lane_default(), name="main"
    )
    stack = rm.edit.EditStack()

    # A free-form solid marking curve, gently drifting across t.
    centerline = [(s, 0.5 * (s - 10.0) / 10.0) for s in range(10, 40, 2)]
    curve_params = rm.edit.MarkingCurveParams()
    curve_params.width_m = 0.2
    curve_params.material = "material.paint_white"
    curve_params.asset = "marking.solid_white"
    curve = rm.Object()
    curve.odr_id = "curve1"
    curve = rm.edit.apply_marking_curve_asset(curve, centerline, curve_params)
    stack.push(network, rm.edit.add_object(network, road, curve))
    print(f"drew a marking curve with {len(curve.marking_curve.samples)} samples")

    # The six core arrow stencils, one every 12 m in the right driving lane.
    for i, subtype in enumerate(ARROWS):
        params = rm.edit.StencilParams()
        params.subtype = subtype
        params.length_m = 4.0
        params.width_m = 1.75
        params.material = "material.paint_white"
        params.asset = f"stencil.{subtype}"
        stencil = rm.Object()
        stencil.odr_id = f"arrow{i}"
        stencil = rm.edit.apply_stencil_asset(stencil, params)
        stencil.s = 50.0 + (i * 12.0)
        stencil.t = -1.75  # centred on the right lane
        stack.push(network, rm.edit.add_object(network, road, stencil))
    print(f"placed {len(ARROWS)} arrow stencils")

    findings = [f for f in rm.validate_network(network) if f.severity == rm.Severity.ERROR]
    if findings:
        for finding in findings:
            print(f"  validation error: {finding.message}", file=sys.stderr)
        return 1

    rm.save_xodr(network, xodr_path, name="Curve Markings + Stencils Demo")
    print(f"wrote {xodr_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
