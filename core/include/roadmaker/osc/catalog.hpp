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

/// The actor archetypes a scenario can be authored from (p8-s2, issue #246).
///
/// WHY THIS IS KERNEL-SIDE AND NOT A WIDGET'S TABLE. Placing an actor has to be
/// replayable headlessly — `python/CMakeLists.txt:36` links `roadmaker::core`
/// alone, so a catalogue living in the editor could never produce the `.xosc`
/// GW-6 fingerprints. It is also the `signs::catalog()` shape (p6-s12), which
/// replaced three identity if-chains with one table: a `ScenarioObject` built
/// by the Actor tool, by a Python replay and by a test must be the SAME bytes,
/// and one table is what makes that structural rather than hoped-for.
///
/// WHERE THE NUMBERS COME FROM. `docs/domain/realism_defaults.md` §1.8, whose
/// table is RENDERED from this file by `actor_catalog_markdown()` and compared
/// against the committed document by a test — the p2-s11 doc-vs-code gate, so a
/// value edited in one place and not the other fails CI rather than review. The
/// car is pinned to §1.1's AASHTO P design vehicle, the anchor every other
/// default in that document is already measured against.
///
/// Reference: ASAM OpenSCENARIO XML 1.4.0 §7.4 (entities), §7.4.1 (Vehicle,
/// Pedestrian). The specification text is NOT tracked in this repository; fetch
/// it with `scripts/fetch_asam_specs.py --std openscenario`.

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace roadmaker::osc {

/// The actor kinds RoadMaker can author.
///
/// NOT a mirror of OpenSCENARIO's `vehicleCategory` enumeration, which has ten
/// values: this is the list a user can pick from a toolbar, and `trailer`,
/// `semitrailer`, `train` and `tram` are not placeable on their own. A foreign
/// file's category still round-trips — `Vehicle::category` is a free string —
/// so nothing here narrows what can be READ.
enum class ActorKind : std::uint8_t {
  Car,
  Truck,
  Bus,
  Motorbike,
  Bicycle,
  Pedestrian,
};

/// One placeable actor archetype: everything needed to build a complete
/// `<ScenarioObject>` without asking the caller for a single dimension.
struct ActorArchetype {
  ActorKind kind;

  /// The stable identifier used in file names, Python, and the `@name` stem the
  /// editor mints from (`"car"` -> `Car1`, `Car2`, …). Lowercase ASCII, never
  /// localized — it reaches the `.xosc`.
  std::string_view key;

  /// The human-readable label a toolbar or Library entry shows.
  std::string_view label;

  /// `Vehicle/@vehicleCategory` or `Pedestrian/@pedestrianCategory`.
  std::string_view category;

  /// True when this archetype builds a `<Pedestrian>` rather than a `<Vehicle>`
  /// — the choice `ScenarioObject::entity_object` makes. `<Pedestrian>` has no
  /// `<Performance>` and no `<Axles>` in any revision, which is why the axle and
  /// performance fields below are ignored for one.
  bool pedestrian = false;

  // --- <BoundingBox><Dimensions> [m] ---------------------------------------
  double width = 0.0;
  double length = 0.0;
  double height = 0.0;

  /// `@mass` [kg].
  double mass = 0.0;

  /// `<BoundingBox><Center>` x [m] — the body centre measured from the entity's
  /// REFERENCE POINT, which for a vehicle is the centre of the rear axle and
  /// not the centre of the body. Getting this wrong does not move the actor; it
  /// makes the drawn box straddle the wrong end of it, which is exactly the
  /// class of error a viewport renders convincingly. `center_z` is always
  /// `height / 2` and is derived rather than stored.
  double center_x = 0.0;

  // --- <Axles> [m], ignored when `pedestrian` -------------------------------

  /// Front axle `@positionX` — the wheelbase, since the rear axle sits at the
  /// origin by definition of the reference point.
  double wheelbase = 0.0;
  double wheel_diameter = 0.0;
  double max_steering = 0.0;

  // --- <Performance>, ignored when `pedestrian` -----------------------------
  double max_speed = 0.0;        ///< [m/s]
  double max_acceleration = 0.0; ///< [m/s^2]
  double max_deceleration = 0.0; ///< [m/s^2]
};

/// Every archetype, in display order (the order a toolbar lists them). Stable:
/// callers index into it and the Actor tool's toolbar order is generated from
/// it.
[[nodiscard]] RM_API std::span<const ActorArchetype> actor_catalog();

/// The archetype for `kind`. Every enumerator has exactly one row (asserted in
/// the tests), so this never fails.
[[nodiscard]] RM_API const ActorArchetype& actor_archetype(ActorKind kind);

/// The archetype whose `key` is `key`, or nullptr — the lookup a persisted
/// string (a tool's last-used kind, a Python argument) needs.
[[nodiscard]] RM_API const ActorArchetype* actor_archetype_by_key(std::string_view key);

/// A complete `<ScenarioObject>` for `kind`, named `name`.
///
/// Fully formed: bounding box, `<Performance>` and `<Axles>` for a vehicle,
/// mass and bounding box for a pedestrian. `write_xosc` accepts the result
/// as-is — that is the point, since `<Performance>` and `<Axles>` are required
/// children of `<Vehicle>` in every revision and a caller assembling them by
/// hand is a caller that will one day forget one.
///
/// An EMPTY `name` is allowed here and refused by the edit factories: this
/// function builds a value, and validating it is `add_scenario_object`'s job,
/// which is where the uniqueness half lives too.
[[nodiscard]] RM_API ScenarioObject make_actor(ActorKind kind, std::string name);

/// The catalogue rendered as the body of `docs/domain/realism_defaults.md`
/// §1.8, marker comment first.
///
/// The doc-vs-code gate (p2-s11): a test asserts the committed document
/// contains exactly this text, so an archetype whose dimensions change in code
/// and not in the document fails CI. Rendered here rather than in
/// `roadmaker::defaults` because `defaults` must not depend on `osc` — every
/// dependency in this tree runs `osc` -> `road`, never back.
[[nodiscard]] RM_API std::string actor_catalog_markdown();

} // namespace roadmaker::osc
