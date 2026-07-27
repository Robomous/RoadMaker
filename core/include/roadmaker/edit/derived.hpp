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

#include "roadmaker/road/id.hpp"

#include <cstddef>
#include <string>

namespace roadmaker::edit {

/// What a move did to one derived layer (cascade-s3, #463).
enum class DerivedChange {
  /// A move closed a loop, so a new Derived ground surface fills it.
  SurfaceAdded,
  /// A move opened a loop, so its Derived surface no longer has a face. Undo
  /// brings it back with its id and its material intact.
  SurfaceRemoved,
  /// An AUTHORED boundary's provenance roads moved and the boundary did NOT.
  /// It is the user's own geometry, so a move must never re-derive it — this
  /// record is the whole of the response, and exists so "nothing happened" is
  /// said out loud rather than looking like an oversight.
  AuthoredBoundaryStale,
  /// A `<bridge>` span was shifted along its road to stay over the crossing it
  /// was built for.
  BridgeRelocated,
  /// A `<bridge>` span's crossing is gone — the roads no longer cross, a
  /// junction now connects them, the clearance closed, or the span cannot be
  /// shifted far enough to reach the new crossing. The span is left EXACTLY as
  /// authored: deleting it would make an ordinary move destroy user data.
  BridgeOrphaned,
};

/// What became of one derived layer a move disturbed (cascade-s3, #463).
///
/// Roads and junctions follow a move (cascade-s1/s2); the layers derived FROM
/// them either follow too or cannot, and the second outcome must never be
/// silent. Which fields carry meaning depends on `change`:
///
/// | change                  | surface | road   | bridge_index |
/// |-------------------------|---------|--------|--------------|
/// | SurfaceAdded/Removed    | set     | —      | —            |
/// | AuthoredBoundaryStale   | set     | moved  | —            |
/// | BridgeRelocated/Orphaned| —       | carrier| set          |
///
/// Read them back through Command::derived_records() after a successful
/// apply(), or EditStack::last_derived_records() headlessly.
/// See docs/domain/connection_contract.md §derived-layer recompute on move.
struct DerivedRecord {
  DerivedChange change = DerivedChange::SurfaceAdded;

  /// The ground surface this record is about, when there is one.
  SurfaceId surface;

  /// The road this record is about: the carrying road for a bridge span, or
  /// the moved provenance road for a stale authored boundary.
  RoadId road;

  /// Index into `Road::bridges`. Only meaningful for the two bridge changes —
  /// a `Bridge` has no strong id, so `(road, index)` is its whole identity.
  std::size_t bridge_index = 0;

  /// Human-readable detail, for the toast and the log. Empty for the changes
  /// that carry no explanation beyond their kind.
  std::string detail;
};

} // namespace roadmaker::edit
