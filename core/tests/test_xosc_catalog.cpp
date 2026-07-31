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

// The actor archetype catalogue (p8-s2, issue #246) — osc/catalog.hpp.
//
// TWO KINDS OF ASSERTION LIVE HERE and they are worth telling apart:
//
//   1. STRUCTURAL — every enumerator has a row, keys are unique, and a
//      `make_actor` result is a scenario object `write_xosc` accepts. These
//      catch a row added to the enum and forgotten in the table.
//   2. THE DOC-VS-CODE GATE — `docs/domain/realism_defaults.md` §1.8 must be
//      exactly what `actor_catalog_markdown()` renders, and the Car row must be
//      §1.1's reference vehicle. These catch a dimension edited in one place
//      and not the other, which is the p2-s11 failure mode this repository has
//      already been bitten by once.

#include "roadmaker/osc/catalog.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/osc/writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace osc = roadmaker::osc;

namespace {

/// Every enumerator, so a new one added to `ActorKind` and not to the table
/// fails here rather than returning the Car row silently.
const std::vector<osc::ActorKind>& all_kinds() {
  static const std::vector<osc::ActorKind> kinds = {
      osc::ActorKind::Car,
      osc::ActorKind::Truck,
      osc::ActorKind::Bus,
      osc::ActorKind::Motorbike,
      osc::ActorKind::Bicycle,
      osc::ActorKind::Pedestrian,
  };
  return kinds;
}

/// Reads a committed Markdown page as TEXT, not as bytes.
///
/// ★ NO std::ios::binary, and that is the whole difference between this passing
/// and failing on Windows. `realism_defaults.md` is a HUMAN-EDITED document, so
/// a Windows checkout gives it CRLF line endings (autocrlf) while
/// `actor_catalog_markdown()` renders '\n' — and a binary read leaves the '\r'
/// in place, so the substring search fails on line endings alone. It failed
/// exactly that way on the first push of p8-s2 (#246), green on macOS and Linux.
///
/// Text mode is the fix rather than a `.gitattributes` LF pin, and the
/// distinction is worth keeping straight: the two existing pins
/// (`editor/resources/help/help.css`, `tests/esmini/*.xosc`) are on GENERATED
/// artifacts whose exact bytes ARE the contract. This document is prose a human
/// edits, its line endings are not a contract, and `test_defaults_registry.cpp`
/// — which compares the same file — already reads it in text mode for the same
/// reason.
std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

TEST(XoscCatalog, EveryKindHasExactlyOneRow) {
  EXPECT_EQ(osc::actor_catalog().size(), all_kinds().size())
      << "the table and the enum disagree on how many archetypes exist";

  std::set<std::string> keys;
  for (const osc::ActorKind kind : all_kinds()) {
    const osc::ActorArchetype& archetype = osc::actor_archetype(kind);
    EXPECT_EQ(archetype.kind, kind)
        << "actor_archetype() fell through to the wrong row — a kind is missing from the table";
    EXPECT_FALSE(archetype.key.empty());
    EXPECT_FALSE(archetype.label.empty());
    EXPECT_FALSE(archetype.category.empty());
    EXPECT_TRUE(keys.insert(std::string(archetype.key)).second)
        << "duplicate archetype key '" << archetype.key << "'";
  }
}

TEST(XoscCatalog, EveryKeyRoundTripsThroughTheLookup) {
  for (const osc::ActorArchetype& archetype : osc::actor_catalog()) {
    const osc::ActorArchetype* found = osc::actor_archetype_by_key(archetype.key);
    ASSERT_NE(found, nullptr) << archetype.key;
    EXPECT_EQ(found->kind, archetype.kind);
  }
  EXPECT_EQ(osc::actor_archetype_by_key("spaceship"), nullptr);
  EXPECT_EQ(osc::actor_archetype_by_key(""), nullptr);
}

TEST(XoscCatalog, EveryArchetypeHasPositiveDimensions) {
  for (const osc::ActorArchetype& archetype : osc::actor_catalog()) {
    EXPECT_GT(archetype.width, 0.0) << archetype.key;
    EXPECT_GT(archetype.length, 0.0) << archetype.key;
    EXPECT_GT(archetype.height, 0.0) << archetype.key;
    EXPECT_GT(archetype.mass, 0.0) << archetype.key;
  }
}

TEST(XoscCatalog, TheCarIsTheDocumentsReferenceVehicle) {
  // ★ §1.1's AASHTO P design vehicle is the proportional anchor EVERY other
  // default in docs/domain/realism_defaults.md is measured against — lane
  // widths, clearances, sign heights. An actor that disagreed with it would
  // make all of them read wrong, so it is pinned here rather than left to
  // whoever next edits the table.
  const osc::ActorArchetype& car = osc::actor_archetype(osc::ActorKind::Car);
  EXPECT_DOUBLE_EQ(car.width, 2.13);
  EXPECT_DOUBLE_EQ(car.length, 5.79);
  EXPECT_DOUBLE_EQ(car.height, 1.45);
}

TEST(XoscCatalog, AVehicleActorIsCompleteEnoughToWrite) {
  // <Performance> and <Axles> are REQUIRED children of <Vehicle> in every
  // revision. The point of make_actor is that a caller never has to know that,
  // so this asserts the result is writable as-is rather than merely populated.
  for (const osc::ActorArchetype& archetype : osc::actor_catalog()) {
    if (archetype.pedestrian) {
      continue;
    }
    osc::Scenario scenario;
    scenario.entities.scenario_objects.push_back(
        osc::make_actor(archetype.kind, std::string(archetype.key) + "1"));

    const auto written = osc::write_xosc(scenario);
    ASSERT_TRUE(written.has_value())
        << archetype.key << ": " << written.error().message << " @ " << written.error().context;

    const osc::ScenarioObject& object = scenario.entities.scenario_objects[0];
    const auto* vehicle = std::get_if<osc::Vehicle>(&object.entity_object);
    ASSERT_NE(vehicle, nullptr) << archetype.key << " did not build a <Vehicle>";
    EXPECT_EQ(vehicle->category, archetype.category);
    EXPECT_DOUBLE_EQ(vehicle->bounding_box.width, archetype.width);
    EXPECT_DOUBLE_EQ(vehicle->bounding_box.length, archetype.length);
    EXPECT_DOUBLE_EQ(vehicle->bounding_box.height, archetype.height);
    // The reference point is on the ground, so the body centre is half a body
    // up — never 0, which would sink the drawn box into the road.
    EXPECT_DOUBLE_EQ(vehicle->bounding_box.center_z, archetype.height / 2.0);
    // The rear axle IS the reference point.
    EXPECT_DOUBLE_EQ(vehicle->axles.rear.position_x, 0.0);
    EXPECT_GT(vehicle->axles.front.position_x, 0.0) << "a vehicle with no wheelbase";
  }
}

TEST(XoscCatalog, APedestrianActorIsAPedestrianAndNotAVehicle) {
  osc::Scenario scenario;
  scenario.entities.scenario_objects.push_back(
      osc::make_actor(osc::ActorKind::Pedestrian, "Pedestrian1"));

  const auto written = osc::write_xosc(scenario);
  ASSERT_TRUE(written.has_value()) << written.error().message;

  const osc::ScenarioObject& object = scenario.entities.scenario_objects[0];
  ASSERT_NE(std::get_if<osc::Pedestrian>(&object.entity_object), nullptr)
      << "a pedestrian archetype built a <Vehicle>, which has <Axles> a pedestrian cannot have";
  EXPECT_EQ(written->find("<Vehicle"), std::string::npos) << *written;
  EXPECT_NE(written->find("<Pedestrian"), std::string::npos) << *written;
}

TEST(XoscCatalog, TheEntityNameAndTheModelNameAreDifferentThings) {
  // <Vehicle @name> names the MODEL and two entities may share it; the
  // <ScenarioObject @name> is the key every entityRef resolves through and must
  // not be. Conflating them would make a scenario with Car1..Car9 declare nine
  // models instead of nine instances of one.
  const osc::ScenarioObject first = osc::make_actor(osc::ActorKind::Car, "Car1");
  const osc::ScenarioObject second = osc::make_actor(osc::ActorKind::Car, "Car2");

  EXPECT_EQ(first.name, "Car1");
  EXPECT_EQ(second.name, "Car2");
  EXPECT_EQ(std::get<osc::Vehicle>(first.entity_object).name,
            std::get<osc::Vehicle>(second.entity_object).name);
}

TEST(XoscCatalog, TheDocumentsActorTableIsExactlyWhatTheCatalogueRenders) {
  // The p2-s11 doc-vs-code gate. A dimension changed in catalog.cpp and not in
  // the document fails HERE, in CI, rather than in review — which is the whole
  // reason the table is rendered rather than written twice.
  const std::filesystem::path doc =
      std::filesystem::path(RM_DOCS_DIR) / "domain" / "realism_defaults.md";
  const std::string text = read_file(doc);
  ASSERT_FALSE(text.empty()) << "could not read " << doc;

  const std::string generated = osc::actor_catalog_markdown();
  EXPECT_NE(text.find(generated), std::string::npos)
      << "docs/domain/realism_defaults.md §1.8 is out of date with osc::actor_catalog_markdown():"
         "\n\n"
      << generated;
}

TEST(XoscCatalog, TheRenderedTableCarriesItsMarker) {
  // Every renderer must emit its marker comment so the document's tables stay
  // findable — the convention roadmaker::defaults already holds itself to.
  EXPECT_EQ(osc::actor_catalog_markdown().rfind("<!-- rm-defaults: actors -->", 0), 0U);
}
