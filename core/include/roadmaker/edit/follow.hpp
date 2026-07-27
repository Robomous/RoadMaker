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

#include "roadmaker/road/road.hpp"

#include <string>

namespace roadmaker::edit {

/// What became of one joint a move disturbed.
enum class FollowOutcome {
  /// The neighbour's contacting end was refit, so the joint still satisfies the
  /// connection contract.
  Followed,
  /// The neighbour could not be refit, so the link was cleared on both sides.
  Severed,
};

/// The record of one joint a move disturbed (cascade-s1, #461).
///
/// A move that changes a road end either takes its linked neighbour with it or
/// severs the link — and the second outcome must never be silent, which is the
/// whole reason this type exists. Every gesture that can outlive a joint emits
/// one record per joint it actually disturbed; a joint that was already
/// continuous, or stayed continuous, produces nothing.
///
/// Read them back through Command::follow_records() after a successful apply().
/// See docs/domain/connection_contract.md §neighbour follow on move.
struct FollowRecord {
  /// The end of the EDITED road — the one the gesture moved.
  RoadEnd moved;
  /// The end that followed it, or whose link was cleared.
  RoadEnd neighbour;
  FollowOutcome outcome = FollowOutcome::Followed;
  /// Why the neighbour could not follow. Empty when it did.
  std::string reason;
};

} // namespace roadmaker::edit
