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

namespace roadmaker {

/// `e_trafficRule` — the road's basic rule of use (OpenDRIVE 1.9.0 §10.2,
/// Table 23: `<road @rule>`). The attribute is optional and *"when this
/// attribute is missing, RHT is assumed"*, which is why the enumeration has no
/// `Unspecified` state and `Road::rule` defaults to `RightHandTraffic`: the
/// spec supplies the default, so an absent attribute and an explicit `RHT` are
/// the same road.
///
/// This lives in a leaf header of its own so `road.hpp` (which owns the field)
/// and `lane.hpp` (which owns `lane_travels_with_s`, the one place the
/// RHT/LHT flip is written down) can both have it without `road.hpp` having to
/// include `lane.hpp`.
enum class TrafficRule {
  RightHandTraffic,
  LeftHandTraffic,
};

} // namespace roadmaker
