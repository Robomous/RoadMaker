// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/poly3.hpp"

#include <span>
#include <vector>

namespace roadmaker {

/// Fits a C1 piecewise-cubic profile z(s) through the sample points
/// (s[i], z[i]) using cubic Hermite interpolation, with node tangents taken
/// from finite differences of the neighbouring samples (central differences
/// interior, one-sided at the ends). The result is one `Poly3` per interval
/// [s[i], s[i+1]] with `Poly3::s == s[i]`; `eval_profile(result, s[i])`
/// reproduces `z[i]` exactly and the pieces meet with matching value and
/// slope (C1). It is intentionally NOT overshoot-limited (no Fritsch-Carlson
/// monotone clamping) — the M2 elevation editor wants a visually smooth curve
/// without that complexity (docs/design/m2/02_editing_tools.md §5).
///
/// `s` must be strictly ascending; `s` and `z` must have equal size. Records
/// come out ascending in s by construction
/// (asam.net:xodr:1.4.0:road.elevation.elem_asc_order). Degenerate inputs:
/// empty → {}, a single sample → one constant record {.s = s[0], .a = z[0]}.
/// A non-ascending or size-mismatched input yields {} (the caller guarantees
/// well-formed stations; this is a defensive floor, not a diagnostic path).
[[nodiscard]] RM_API std::vector<Poly3> fit_elevation_profile(std::span<const double> s,
                                                              std::span<const double> z);

/// As above but with EXPLICIT node tangents m[i] = dz/ds — the profile
/// editor's grade handles (hardening sprint, docs/design/hardening
/// workstream C). Same conventions and degenerate handling; `m` must match
/// `s` in size.
[[nodiscard]] RM_API std::vector<Poly3> fit_elevation_profile(std::span<const double> s,
                                                              std::span<const double> z,
                                                              std::span<const double> m);

} // namespace roadmaker
