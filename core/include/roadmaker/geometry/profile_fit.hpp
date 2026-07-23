/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
