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

#include "roadmaker/xodr/raw_xml.hpp"

#include <string>

namespace roadmaker {

/// A `<bridge>` span (ASAM OpenDRIVE 1.9.0 §13.12, `t_road_objects_bridge`)
/// inside a road's `<objects>`, multiplicity 0..* (p5-s3, #233).
///
/// The load-bearing split (docs/design/materials-structures/02_bridge_generator.md
/// §2): the SPAN is standard and serialized — this record — while the SOLIDS
/// (deck/abutments/piers/guardrails) are Manifold geometry generated
/// deterministically at load and NEVER written, because OpenDRIVE has no
/// vocabulary for them. Same span + same road geometry + same generator
/// parameters ⇒ same solids, so nothing is lost by not storing them and the
/// round-trip stays byte-identical.
///
/// Before this sprint a `<bridge>` was preserved verbatim in
/// `Road::object_extras`; it is now a first-class record and the reader routes
/// it here instead.
struct Bridge {
  /// @id (required by the schema; a foreign file missing it still loads with a
  /// warning and an empty id).
  std::string odr_id;

  /// @name (optional).
  std::string name;

  /// @s — the span start in the carrying road's s-coordinate, `t_grEqZero`.
  double s = 0.0;

  /// @length — the span length along s, `t_grEqZero`.
  double length = 0.0;

  /// @type — `e_bridgeType` (concrete / steel / wood / brick). v1 authors
  /// "concrete"; the others parse and round-trip, but the generator's solids do
  /// not vary by type. The ORIGINAL spelling survives even for an out-of-enum
  /// value — this is a plain string, not an enum, so nothing is lost.
  std::string type = "concrete";

  /// The surface material for the deck, carried on a
  /// `<userData code="rm:material.bridge_deck" value="...">` child (design §2 —
  /// no standard carrier exists). Empty when the bridge names no material.
  std::string deck_material;

  /// Unmodeled attributes and unmodeled children — notably a `<laneValidity>`
  /// that narrows the bridge to a lane subset
  /// (asam.net:xodr:1.7.0:road.object.bridges.define_type). v1 always spans the
  /// full cross-section and writes no `<laneValidity>`, but a foreign one is
  /// preserved verbatim and re-emitted so round-trip loses nothing.
  RawXml extras;

  friend bool operator==(const Bridge&, const Bridge&) = default;
};

} // namespace roadmaker
