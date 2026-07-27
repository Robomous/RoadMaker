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

#include "roadmaker/mesh/prop_obstructions.hpp"
#include "roadmaker/road/id.hpp"

#include <string>

namespace roadmaker::edit {

/// A prop a move drove into something (cascade-s4, #464).
///
/// Reported, never corrected. Props follow their anchor road's frame, which is
/// correct and is exactly what can carry one into another road, a junction
/// floor or another prop — most sharply when the anchor road is ROTATED and a
/// prop at large |t| sweeps a wide arc (#338, whose risk note this closes).
///
/// ONLY NEW OBSTRUCTIONS APPEAR HERE. The move funnel reads the obstruction set
/// before the gesture and reports the difference, so nudging a road near an
/// imported network that already has a tree in a lane says nothing about a
/// state the user did not create. Same stance as an already-broken joint in
/// cascade-s1.
///
/// The offered fix is edit::relocate_obstructed_props, which the user invokes
/// explicitly — a move never silently moves a prop.
///
/// Read these back through Command::obstruction_records() after a successful
/// apply(), or EditStack::last_obstruction_records() headlessly.
/// See docs/domain/connection_contract.md §prop obstruction on move.
struct ObstructionRecord {
  /// What the prop ran into, and where — object, instance, kind, the thing
  /// obstructed, and a witness point inside both shapes.
  PropObstruction obstruction;

  /// The prop's OpenDRIVE id, so a report can name it without a second lookup
  /// (a Diagnostic and a toast both quote it, and by then the network may have
  /// moved on).
  std::string object_odr_id;

  /// Human-readable detail for the toast and the log: what it hit, by name.
  std::string detail;
};

} // namespace roadmaker::edit
