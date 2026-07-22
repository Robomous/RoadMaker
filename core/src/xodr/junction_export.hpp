// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Internal (non-installed) header: derives the OpenDRIVE ≥1.8 junction surface
// elements — the <planView> reference line and the <elevationGrid> — from a
// junction's blended 2.5D surface. The writer serializes the returned struct;
// the geometry is a pure function of network topology so it is regenerated on
// every write rather than stored in the model
// (docs/design/m2/03_junction_blending.md §3).
//
// The <boundary> (§12.10) is derived by build_junction_boundary below (M3a
// phase 2b, #62): a CCW closed loop of lane/joint segments. Where an adjacent
// arm pair has no bridging connecting road, the boundary is closed with a
// synthesized auxiliary boundary road (junctions.boundary.close_gap_with_new_roads,
// spec Fig. 99): a derived <road @junction> the writer emits among the real
// roads (tagged rm:aux_boundary so the reader drops it — round-trip stays a
// fixed point) whose outer edge provides the closing lane segment. Only a
// foreign junction with no arm metadata still leaves the boundary unwritten and
// keeps validate_network's warning. The <elevationGrid> is spec-valid with or
// without a <boundary>.

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <string>
#include <vector>

namespace roadmaker {

struct Junction;

/// The junction reference line: one straight <geometry>/<line> along the
/// footprint's principal axis, extended one grid square past the surface so a
/// perpendicular from it reaches every point of the junction
/// (junctions.geometry.only_one_line_element, …ref_line_definition).
struct JunctionRefLine {
  double x = 0.0;   // start position (kernel frame, metres)
  double y = 0.0;   //
  double hdg = 0.0; // heading of the line [rad]
  double length = 0.0;
};

/// One column of the elevation grid, perpendicular to the reference line.
/// `center` is z on the reference line; `left`/`right` list z from inside
/// (nearest the line) to outside, matching the <elevation> attribute order.
struct JunctionGridColumn {
  std::vector<double> left;
  double center = 0.0;
  std::vector<double> right;
};

/// A rectangular elevation grid sampled from the harmonic surface, valid over
/// the whole junction boundary (junctions.elevation_grid.*). Columns are
/// spaced `grid_spacing` along the reference line starting at `s_start`; each
/// column carries the same left/right count (a full rectangular grid, so the
/// spec's bicubic reconstruction always has its support points).
struct JunctionElevationGrid {
  double s_start = 0.0;
  double grid_spacing = 0.0;
  std::vector<JunctionGridColumn> columns;
};

/// The derived ≥1.8 surface export for one junction. `has_surface` is false
/// when the junction has no usable footprint (no reference line / grid is
/// written). The <boundary> is a separate export (build_junction_boundary).
struct JunctionSurfaceExport {
  bool has_surface = false;
  JunctionRefLine ref_line;
  JunctionElevationGrid grid;
};

/// Builds the surface export for `junction`. Pure and deterministic (it runs
/// the same build_junction_surface pipeline the mesher uses, then samples it),
/// so repeated writes of the same network are byte-identical.
[[nodiscard]] JunctionSurfaceExport build_junction_export(const RoadNetwork& network,
                                                          const Junction& junction,
                                                          const SamplingOptions& sampling = {});

/// One <segment> of a junction <boundary> (§12.10). A `lane` segment runs
/// along a connecting road's outer lane edge; a `joint` segment caps an
/// incoming road perpendicular to its lanes.
struct JunctionBoundarySegment {
  bool is_lane = true; ///< true = <segment type="lane">, false = <segment type="joint">
  std::string road_id; ///< @roadId — connecting road (lane) or arm road (joint)
  int boundary_lane =
      0; ///< @boundaryLane (lane segments) — the lane whose outer edge is the segment
  ContactPoint contact = ContactPoint::Start; ///< @contactPoint (joint segments)
  /// Lane segments walk @sStart→@sEnd along the connecting road; true emits
  /// "begin"→"end", false "end"→"begin" (the CCW walk may reverse a road).
  bool s_begin_to_end = true;
};

/// A synthesized auxiliary boundary road (§12.10, junctions.boundary.
/// close_gap_with_new_roads, spec Fig. 99): a derived helper road that exists
/// solely to provide the closing `<segment type="lane" boundaryLane="0">` where
/// an adjacent arm pair leaves a gap no existing lane edge can close. It is not
/// stored in the model — the writer emits it as a `<road @junction>` carrying a
/// `rm:aux_boundary` userData marker, and the reader drops any road bearing that
/// marker so write→parse→write is a byte-identical fixed point.
struct AuxBoundaryRoad {
  std::string odr_id;          ///< synthesized @id, deterministic per gap
  std::string junction_odr_id; ///< @junction (the owning junction's id)
  double x = 0.0;              ///< reference-line start (kernel frame, metres)
  double y = 0.0;              ///<
  double hdg = 0.0;            ///< heading of the single <line> [rad]
  double length = 0.0;         ///< corner-to-corner length [m]
  std::string pred_road;       ///< <link> predecessor road @id (arm a)
  ContactPoint pred_contact = ContactPoint::Start;
  std::string succ_road; ///< <link> successor road @id (arm b)
  ContactPoint succ_contact = ContactPoint::Start;
};

/// The derived junction <boundary> (§12.10): a counter-clockwise, closed loop
/// of lane/joint segments for a common junction. `aux_roads` carries any
/// synthesized auxiliary boundary roads whose lane segments close gaps (the
/// writer emits them among the real roads). `has_boundary` is false only when
/// the boundary cannot be closed at all — a foreign junction with no arm
/// metadata — and the writer then omits <boundary> and validate_network keeps
/// the close_gap_with_new_roads warning.
struct JunctionBoundaryExport {
  bool has_boundary = false;
  std::vector<JunctionBoundarySegment> segments;
  std::vector<AuxBoundaryRoad> aux_roads;
};

/// Derives the junction <boundary> from its connecting roads: collects the
/// arms, orders them counter-clockwise around the footprint centroid, and
/// walks the outer connecting road between each adjacent arm pair (a `lane`
/// segment) with a `joint` cap at each arm. An adjacent pair with no bridging
/// connecting road is closed by synthesizing an auxiliary boundary road along
/// the outer edge between the two arm mouths (appended to `aux_roads`). Pure and
/// deterministic. Returns has_boundary=false only when a connecting road has no
/// arm metadata (a foreign junction) or the junction has no usable connecting
/// roads.
[[nodiscard]] JunctionBoundaryExport build_junction_boundary(const RoadNetwork& network,
                                                             const Junction& junction);

} // namespace roadmaker
