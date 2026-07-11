#pragma once

// Internal (non-installed) header: derives the OpenDRIVE ≥1.8 junction surface
// elements — the <planView> reference line and the <elevationGrid> — from a
// junction's blended 2.5D surface. The writer serializes the returned struct;
// the geometry is a pure function of network topology so it is regenerated on
// every write rather than stored in the model
// (docs/design/m2/03_junction_blending.md §3).
//
// <boundary> is NOT emitted in M2: closing it with lane/joint segments and,
// where arms leave a gap, generating auxiliary boundary roads
// (junctions.boundary.close_gap_with_new_roads) is the M3 scope. The writer
// omits it and validate_network warns (§3, "keeps the surface editor-internal.
// Auxiliary road generation is M3"). The <elevationGrid> is spec-valid without
// a <boundary>: the valid_for_entire_boundry rule is conditional on one being
// defined.

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"

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
/// written). No <boundary> is produced in M2 (see the file header).
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

} // namespace roadmaker
