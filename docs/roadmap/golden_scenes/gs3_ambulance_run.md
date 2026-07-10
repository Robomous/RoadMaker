# GS-3 — Ambulance run

*Golden scene for M4 (scenario mode), extended in M5 (scenario logic): the
GS-1 map plus one emergency-vehicle actor following a lane-anchored route
through the junction.*

Specified from OpenDRIVE and OpenSCENARIO XML concepts only, per the
[product-parity rules](../../standards/product-parity.md).

## Scene definition

- **Map:** [GS-1 "Urban intersection"](gs1_urban_intersection.md),
  unchanged.
- **Actor:** one emergency vehicle (ambulance-class vehicle entity — an
  OpenSCENARIO `Vehicle` with `vehicleCategory` and boundingBox/performance
  attributes; the 3D model is a CC0 asset from the
  [library](../asset_candidates.md)).
- **Route:** lane-anchored — defined against the road network (road/lane
  ids + s-coordinates, an OpenSCENARIO route/trajectory referencing the
  map), entering on one arm's driving lane, crossing the junction straight
  through via a connecting road, exiting on the opposite arm.
- **Lane offset:** a lateral offset segment along the approach (e.g.
  +0.8 m for ~30 m, as an emergency vehicle straddling toward the center
  line), expressed as the scenario's lane-offset action/property on the
  route.
- **Attributes:** an initial speed attribute on the actor (e.g. 60 km/h)
  set through the actor attributes panel.
- **Authoring flow exercised:** place the actor from the Asset Library
  Browser (drag-to-place), draw the route on the map with lane anchoring,
  set the offset and speed in the attributes panel — all in scenario mode
  after an explicit Map → Scenario mode switch.

## Element checklist (M4)

| # | Element | Depends on |
|---|---|---|
| 1 | ☐ Mode switch Map ↔ Scenario, panels/tools change accordingly | M4 app modes |
| 2 | ☐ Ambulance placed via Library Browser drag-to-place | M4 browser + CC0 vehicle asset |
| 3 | ☐ Route drawn lane-anchored through the junction | M4 route tool + kernel route model |
| 4 | ☐ Route renders as an editable polyline with anchor handles | M4 route rendering/editing |
| 5 | ☐ Lane-offset segment visible in the route's rendered path | offset model + render |
| 6 | ☐ Speed attribute editable in the actor attributes panel | M4 attributes panel |
| 7 | ☐ Scenario saves to `.xosc` and reloads identically | M4 OpenSCENARIO writer/reader |

## Acceptance beyond the image (M4)

- The scenario **round-trips to valid OpenSCENARIO XML 1.4.0**: write →
  validate (rule-id-cited diagnostics) → parse → compare — entities, route,
  offset, and speed survive.
- The route is anchored: moving nothing on the map, reloading the `.xosc`
  reproduces the same s/t geometry; the route references the map by road
  and lane ids, not by world coordinates.
- The `.xodr` + `.xosc` pair loads in the golden-screenshot workflow and
  renders from the fixed camera.

## M5 extension — logic graph

M5 extends this scene rather than adding a fourth:

- A storyboard with one story: the ambulance approaches; a condition
  (reach-position or time headway at the stop line) triggers a maneuver —
  the signal phase yields and the ambulance clears the junction, then the
  offset returns to zero.
- The story/maneuver/condition structure is **authored in the node-based
  logic editor** and renders as a readable graph (states, transitions,
  condition nodes).
- Acceptance: the logic graph round-trips to the same valid `.xosc`, and a
  headless esmini run of the exported scenario completes without errors
  (simulation preview hook).
- Golden screenshot gains a second image: the logic-editor view of the
  story graph, captured at a fixed canvas framing defined when M5's editor
  exists.

## Fixed camera

Same viewport camera as [GS-1](gs1_urban_intersection.md#fixed-camera), so
the map underlay is directly comparable — with the ambulance mid-approach
at the route's offset segment (actor placed at s chosen so it sits ~25 m
before the stop line in frame).

## History

- 2026-07-10 — initial spec (this document).
