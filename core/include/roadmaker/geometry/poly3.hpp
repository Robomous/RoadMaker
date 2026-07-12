#pragma once

#include <algorithm>
#include <span>

namespace roadmaker {

/// Cubic polynomial record as used throughout OpenDRIVE:
///   value(s) = a + b·ds + c·ds² + d·ds³  with  ds = s − this->s.
///
/// `s` is the record's start coordinate. Whether `s` is global along the
/// road or local to a lane section depends on the owning profile — each
/// owner documents its convention. Units: meters in, meters (or radians,
/// for superelevation) out.
struct Poly3 {
  double s = 0.0;
  double a = 0.0;
  double b = 0.0;
  double c = 0.0;
  double d = 0.0;

  [[nodiscard]] constexpr double eval(double s_query) const {
    const double ds = s_query - s;
    return a + (ds * (b + (ds * (c + (ds * d)))));
  }

  /// First derivative d(value)/ds at s_query.
  [[nodiscard]] constexpr double eval_derivative(double s_query) const {
    const double ds = s_query - s;
    return b + (ds * ((2.0 * c) + (ds * 3.0 * d)));
  }

  friend constexpr bool operator==(const Poly3&, const Poly3&) = default;
};

/// Evaluates a piecewise-cubic profile at `s`: picks the last record whose
/// start is <= s (records must be sorted ascending by s). Returns 0.0 for an
/// empty profile; queries before the first record evaluate the first record.
[[nodiscard]] constexpr double eval_profile(std::span<const Poly3> profile, double s) {
  if (profile.empty()) {
    return 0.0;
  }
  auto it = std::upper_bound(
      profile.begin(), profile.end(), s, [](double lhs, const Poly3& rhs) { return lhs < rhs.s; });
  if (it != profile.begin()) {
    --it;
  }
  return it->eval(s);
}

/// First derivative of a piecewise-cubic profile at `s`; same record lookup
/// as eval_profile. Returns 0.0 for an empty profile.
[[nodiscard]] constexpr double eval_profile_derivative(std::span<const Poly3> profile, double s) {
  if (profile.empty()) {
    return 0.0;
  }
  auto it = std::upper_bound(
      profile.begin(), profile.end(), s, [](double lhs, const Poly3& rhs) { return lhs < rhs.s; });
  if (it != profile.begin()) {
    --it;
  }
  return it->eval_derivative(s);
}

} // namespace roadmaker
