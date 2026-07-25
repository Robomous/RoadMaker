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

namespace roadmaker::fill_params {

/// Interior Steiner spacing [m] of the surface fill — junction floors and
/// enclosed ground surfaces alike (docs/design/m2/03_junction_blending.md §3,
/// "~1–2 m"): grid points that give the harmonic membrane room to bend between
/// the road edges.
///
/// Public because it is the FEATURE SIZE of every fill, so tolerances derived
/// from it belong with it rather than being restated as bare numbers. The
/// sidewalk-band seam accuracy of issue #402 is one such tolerance: a
/// classification that is not constrained to the seam is wrong by about one
/// triangle, and this is how big a triangle is.
inline constexpr double kSteinerStep = 2.0;

} // namespace roadmaker::fill_params
