#include "roadmaker/geometry/profile_fit.hpp"

#include "roadmaker/tol.hpp"

#include <cstddef>

namespace roadmaker {

std::vector<Poly3> fit_elevation_profile(std::span<const double> s, std::span<const double> z) {
  const std::size_t n = s.size();
  if (n != z.size() || n == 0) {
    return {};
  }
  if (n == 1) {
    return {Poly3{.s = s[0], .a = z[0]}};
  }
  // Reject non-ascending stations defensively: a zero/negative run would make
  // the finite-difference tangents blow up. Callers pass reference-line
  // stations, which are strictly ascending.
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (s[i + 1] - s[i] <= tol::kLength) {
      return {};
    }
  }

  // Node tangents m[i] = dz/ds by finite differences: central in the interior,
  // one-sided at the two ends.
  std::vector<double> m(n);
  m[0] = (z[1] - z[0]) / (s[1] - s[0]);
  m[n - 1] = (z[n - 1] - z[n - 2]) / (s[n - 1] - s[n - 2]);
  for (std::size_t i = 1; i + 1 < n; ++i) {
    m[i] = (z[i + 1] - z[i - 1]) / (s[i + 1] - s[i - 1]);
  }

  // One Hermite cubic per interval, in the element-local coordinate ds = s - s[i]
  // (OpenDRIVE convention, ds restarts at 0 per element). With value/slope
  // endpoints (z[i], m[i]) and (z[i+1], m[i+1]) over run h:
  //   a = z[i], b = m[i]
  //   c = 3·Δ/h² − Δ'/h,  d = Δ'/h² − 2·Δ/h³
  // where Δ = z[i+1] − z[i] − m[i]·h and Δ' = m[i+1] − m[i].
  std::vector<Poly3> profile;
  profile.reserve(n - 1);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double h = s[i + 1] - s[i];
    const double delta = z[i + 1] - z[i] - (m[i] * h);
    const double delta_slope = m[i + 1] - m[i];
    profile.push_back(Poly3{
        .s = s[i],
        .a = z[i],
        .b = m[i],
        .c = ((3.0 * delta) / (h * h)) - (delta_slope / h),
        .d = (delta_slope / (h * h)) - ((2.0 * delta) / (h * h * h)),
    });
  }
  return profile;
}

} // namespace roadmaker
