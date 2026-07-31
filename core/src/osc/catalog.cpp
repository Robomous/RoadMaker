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

/// The actor archetype table (p8-s2, issue #246) — see osc/catalog.hpp for why
/// it is kernel-side and where its numbers come from.

#include "roadmaker/osc/catalog.hpp"

#include <fmt/format.h>

#include <cstddef>
#include <utility>

namespace roadmaker::osc {
namespace {

/// THE table. Ordered as a toolbar lists them: the two everyday vehicles first,
/// then the heavy ones, then the vulnerable road users.
///
/// Vehicle dimensions are AASHTO design vehicles, which is the same source
/// `docs/domain/realism_defaults.md` §1.1 anchors every other default to —
/// P (passenger car), SU-30 (single-unit truck), BUS-40 (city transit bus),
/// and B (bicycle). `center_x` is the body centre measured forward from the
/// rear axle, so a drawn box straddles the vehicle and not its back bumper.
///
/// ★ The Car row is §1.1's reference vehicle EXACTLY (2.13 x 5.79 x 1.45) and a
/// test pins it there. It is the proportional anchor the whole defaults
/// document is measured against, so an actor that disagreed with it would make
/// every lane-width and clearance default in that file read wrong.
constexpr ActorArchetype kCatalog[] = {
    ActorArchetype{
        .kind = ActorKind::Car,
        .key = "car",
        .label = "Car",
        .category = "car",
        .pedestrian = false,
        .width = 2.13,
        .length = 5.79,
        .height = 1.45,
        .mass = 1500.0,
        .center_x = 1.9,
        .wheelbase = 2.9,
        .wheel_diameter = 0.65,
        .max_steering = 0.5,
        .max_speed = 69.4,
        .max_acceleration = 5.0,
        .max_deceleration = 10.0,
    },
    ActorArchetype{
        .kind = ActorKind::Truck,
        .key = "truck",
        .label = "Truck",
        .category = "truck",
        .pedestrian = false,
        .width = 2.44,
        .length = 9.14,
        .height = 4.11,
        .mass = 12000.0,
        .center_x = 3.4,
        .wheelbase = 6.1,
        .wheel_diameter = 1.05,
        .max_steering = 0.44,
        .max_speed = 33.3,
        .max_acceleration = 2.0,
        .max_deceleration = 6.0,
    },
    ActorArchetype{
        .kind = ActorKind::Bus,
        .key = "bus",
        .label = "Bus",
        .category = "bus",
        .pedestrian = false,
        .width = 2.59,
        .length = 12.19,
        .height = 3.2,
        .mass = 15000.0,
        .center_x = 4.5,
        .wheelbase = 7.62,
        .wheel_diameter = 1.05,
        .max_steering = 0.44,
        .max_speed = 30.6,
        .max_acceleration = 1.5,
        .max_deceleration = 5.0,
    },
    ActorArchetype{
        .kind = ActorKind::Motorbike,
        .key = "motorbike",
        .label = "Motorbike",
        .category = "motorbike",
        .pedestrian = false,
        .width = 0.8,
        .length = 2.2,
        .height = 1.5,
        .mass = 250.0,
        .center_x = 0.7,
        .wheelbase = 1.4,
        .wheel_diameter = 0.6,
        .max_steering = 0.6,
        .max_speed = 55.6,
        .max_acceleration = 6.0,
        .max_deceleration = 9.0,
    },
    ActorArchetype{
        .kind = ActorKind::Bicycle,
        .key = "bicycle",
        .label = "Bicycle",
        .category = "bicycle",
        .pedestrian = false,
        .width = 0.6,
        .length = 1.8,
        .height = 1.7,
        .mass = 100.0,
        .center_x = 0.55,
        .wheelbase = 1.1,
        .wheel_diameter = 0.68,
        .max_steering = 0.6,
        .max_speed = 13.9,
        .max_acceleration = 2.0,
        .max_deceleration = 4.0,
    },
    // A pedestrian's reference point is the centre of the ground contact area,
    // so `center_x` is 0 rather than a body offset — the one row where it is not
    // measured from an axle, because there is no axle.
    ActorArchetype{
        .kind = ActorKind::Pedestrian,
        .key = "pedestrian",
        .label = "Pedestrian",
        .category = "pedestrian",
        .pedestrian = true,
        .width = 0.6,
        .length = 0.4,
        .height = 1.75,
        .mass = 80.0,
        .center_x = 0.0,
        .wheelbase = 0.0,
        .wheel_diameter = 0.0,
        .max_steering = 0.0,
        .max_speed = 0.0,
        .max_acceleration = 0.0,
        .max_deceleration = 0.0,
    },
};

BoundingBox make_bounding_box(const ActorArchetype& archetype) {
  BoundingBox box;
  box.center_x = archetype.center_x;
  box.center_y = 0.0;
  // Always half the height: an entity's reference point is on the ground, so
  // the body centre is exactly half a body up. Derived rather than stored
  // because there is no archetype for which it could differ.
  box.center_z = archetype.height / 2.0;
  box.width = archetype.width;
  box.length = archetype.length;
  box.height = archetype.height;
  return box;
}

/// Shortest-form metre rendering for the documentation table. Deliberately not
/// the writer's `num()`: this is prose, and every value in the table is a
/// two-decimal measurement.
std::string len(double metres) {
  return fmt::format("{:.2f}", metres);
}

} // namespace

std::span<const ActorArchetype> actor_catalog() {
  return kCatalog;
}

const ActorArchetype& actor_archetype(ActorKind kind) {
  for (const ActorArchetype& archetype : kCatalog) {
    if (archetype.kind == kind) {
      return archetype;
    }
  }
  // Unreachable: the table covers every enumerator and a test proves it. The
  // fallback keeps the reference valid rather than dereferencing end().
  return kCatalog[0];
}

const ActorArchetype* actor_archetype_by_key(std::string_view key) {
  for (const ActorArchetype& archetype : kCatalog) {
    if (archetype.key == key) {
      return &archetype;
    }
  }
  return nullptr;
}

ScenarioObject make_actor(ActorKind kind, std::string name) {
  const ActorArchetype& archetype = actor_archetype(kind);

  ScenarioObject object;
  object.name = std::move(name);

  if (archetype.pedestrian) {
    Pedestrian pedestrian;
    // <Pedestrian @name> is the MODEL's name, not the entity's — two entities
    // may legitimately share it. It is the archetype label rather than the
    // entity name so a scenario with Pedestrian1..Pedestrian9 declares nine
    // instances of one model instead of nine models.
    pedestrian.name = std::string(archetype.label);
    pedestrian.category = std::string(archetype.category);
    pedestrian.mass = archetype.mass;
    pedestrian.bounding_box = make_bounding_box(archetype);
    object.entity_object = std::move(pedestrian);
    return object;
  }

  Vehicle vehicle;
  vehicle.name = std::string(archetype.label);
  vehicle.category = std::string(archetype.category);
  vehicle.mass = archetype.mass;
  vehicle.bounding_box = make_bounding_box(archetype);

  vehicle.performance.max_speed = archetype.max_speed;
  vehicle.performance.max_acceleration = archetype.max_acceleration;
  vehicle.performance.max_deceleration = archetype.max_deceleration;

  // The rear axle IS the reference point, so its positionX is 0 — the Axle
  // default. Only its wheel geometry varies by archetype.
  vehicle.axles.front.max_steering = archetype.max_steering;
  vehicle.axles.front.wheel_diameter = archetype.wheel_diameter;
  vehicle.axles.front.track_width = archetype.width;
  vehicle.axles.front.position_x = archetype.wheelbase;
  vehicle.axles.front.position_z = archetype.wheel_diameter / 2.0;

  vehicle.axles.rear.max_steering = 0.0;
  vehicle.axles.rear.wheel_diameter = archetype.wheel_diameter;
  vehicle.axles.rear.track_width = archetype.width;
  vehicle.axles.rear.position_x = 0.0;
  vehicle.axles.rear.position_z = archetype.wheel_diameter / 2.0;

  object.entity_object = std::move(vehicle);
  return object;
}

std::string actor_catalog_markdown() {
  std::string out = "<!-- rm-defaults: actors -->\n"
                    "| Actor | Category | Width | Length | Height | Mass |\n"
                    "|---|---|---|---|---|---|\n";
  for (const ActorArchetype& archetype : kCatalog) {
    out += fmt::format("| {} | `{}` | {} m | {} m | {} m | {:.0f} kg |\n",
                       archetype.label,
                       archetype.category,
                       len(archetype.width),
                       len(archetype.length),
                       len(archetype.height),
                       archetype.mass);
  }
  return out;
}

} // namespace roadmaker::osc
