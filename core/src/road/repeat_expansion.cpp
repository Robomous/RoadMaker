// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/road/repeat_expansion.hpp"

#include "roadmaker/road/object.hpp"
#include "roadmaker/tol.hpp"

#include <cmath>

namespace roadmaker {

std::vector<RepeatInstance> expand_repeat(const ObjectRepeat& repeat) {
  std::vector<RepeatInstance> instances;

  // §13.4: "If @distance is set to 0, the object's profile is instead extruded
  // ... creating one continuous shape." A continuous object is not a series of
  // discrete instances, so it produces none here.
  if (repeat.distance <= 0.0) {
    return instances;
  }

  // §13.4: "an instance of the object is repeatedly placed ... as many time as
  // the specified parameters of @distance and @length allow, rounded down."
  // Instances sit at ds = 0, distance, 2*distance, ... k*distance, so k+1 of
  // them. The tol::kLength slack lets an exact-fit endpoint (length == k*
  // distance) survive float rounding — "the origin of each instance ... must be
  // inside that section (including its border)."
  const long k = static_cast<long>(std::floor((repeat.length + tol::kLength) / repeat.distance));

  // §13.4: the cubic branch applies only when >=1 of bT/cT/dT is provided
  // (1.9.0 addition); otherwise t is a linear tStart->tEnd lerp (the sole mode
  // in 1.8.1 §13.2). Absent coefficients count as zero.
  const bool cubic_t = repeat.b_t.has_value() || repeat.c_t.has_value() || repeat.d_t.has_value();
  const double b = repeat.b_t.value_or(0.0);
  const double c = repeat.c_t.value_or(0.0);
  const double d = repeat.d_t.value_or(0.0);

  for (long i = 0; i <= k; ++i) {
    const double ds = static_cast<double>(i) * repeat.distance;
    // Guard length<=0: with distance>0 only the ds=0 instance exists, and the
    // ratio is pinned to 0 to avoid dividing by a zero length.
    const double ratio = repeat.length > 0.0 ? ds / repeat.length : 0.0;

    const double t = cubic_t ? repeat.t_start + b * ds + c * ds * ds + d * ds * ds * ds
                             : repeat.t_start + (repeat.t_end - repeat.t_start) * ratio;
    const double z = repeat.z_offset_start + (repeat.z_offset_end - repeat.z_offset_start) * ratio;

    instances.push_back(RepeatInstance{.s = repeat.s + ds, .t = t, .z_offset = z});
  }
  return instances;
}

} // namespace roadmaker
