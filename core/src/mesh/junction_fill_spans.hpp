// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Internal (non-installed) header: the per-connecting-road grouping of the
// junction floor's fill inputs (p4-s5, issue #320). Junction-only, like
// junction_corner_detail.hpp — surface_fill.cpp (P2 ground surfaces) never sees
// it, and fill_backend.hpp stays the bit-for-bit shared backend it was.
//
// The floor mesher used to flatten every connecting road's contribution into
// shared vectors the moment it gathered them, which left nothing to attribute
// an interior triangulation artifact to. Grouping the contributions per road
// first is what lets the user inspect, exclude and order them (the public
// junction_surface_spans() query is a thin conversion over the same collect).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/network.hpp"

#include <vector>

#include "fill_backend.hpp"

namespace roadmaker {

struct Junction;

namespace junction_fill_spans {

/// One connecting road's grouped contribution to the junction floor union — a
/// "surface span". `included`/`sort_index` are the EFFECTIVE values (the
/// junction's authored SurfaceSpan record merged over the derived defaults);
/// `authored` says whether a record supplied them.
struct JunctionFillSpan {
  RoadId road;

  /// Plan-view footprint (already forced CCW, the winding InflatePaths needs),
  /// exact border ring, and centerline samples — see fill_backend::build_contribution.
  fill_backend::RoadContribution contribution;

  /// false ⇒ this span's SAMPLES leave the fill inputs (elevation Dirichlet,
  /// centerline constraints, boundary-debris protection). Its FOOTPRINT stays
  /// in the union, so coverage and the `<boundary>` export never change.
  bool included = true;

  /// Higher wins where span footprints overlap: the elevation of an overlap
  /// region is taken from the highest-sorted span covering it.
  int sort_index = 0;

  /// True when the junction carries a SurfaceSpan record for this road.
  bool authored = false;
};

/// The junction's fill spans in connection order (junction_corner_detail::
/// connecting_roads), skipping roads that are stale or carry no geometry.
///
/// Deterministic and pure: the ONE place the floor's per-road inputs are built,
/// shared by the mesher and the public query so the two cannot disagree about
/// what a span is.
[[nodiscard]] std::vector<JunctionFillSpan> collect_fill_spans(const RoadNetwork& network,
                                                               const Junction& junction,
                                                               const SamplingOptions& sampling);

} // namespace junction_fill_spans
} // namespace roadmaker
