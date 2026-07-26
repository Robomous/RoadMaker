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

#include "roadmaker/export.hpp"

#include <optional>
#include <span>
#include <string_view>

/// The shipped sign pack, as data.
///
/// docs/domain/realism_defaults.md §1.4 governs the US pack: its face sizes,
/// its mounting geometry, and its persistence. This table is the single place
/// the product answers "what is a stop sign?" — identity authoring
/// (editor/src/document/signal_placement.cpp), mesh selection
/// (core/src/mesh/mesh_builder.cpp), the junction signalize templates
/// (core/src/edit/operations.cpp) and the Library manifest all read it instead
/// of carrying their own if-chain. §1.4's "sign definitions are data, not
/// code" is exactly this: a future country pack is another table, with no
/// engine change. The pack *selection system* (multiple packs, a UI to choose
/// between them) is deliberately not built yet.
///
/// Every dimension comes from roadmaker::defaults (the §1.4 constants) — this
/// file never restates a number, and test_defaults_registry.cpp asserts that.
namespace roadmaker::signs {

/// The face silhouette a designation is drawn with. The mesh generator
/// (scripts/gen_prop_meshes.py) mirrors these shapes; the kernel uses them to
/// reason about the face without owning the geometry.
enum class FaceShape {
  None, ///< not a plated sign (a traffic-light head)
  Octagon,
  TriangleDown,
  Diamond,
  Pentagon,
  Rectangle,
  Disc,
};

/// One catalogue entry: a sign designation plus everything needed to author,
/// draw, and persist it.
struct SignDef {
  /// Library manifest tag and tool token, e.g. "us.r1_1". This is the string
  /// `LibraryItem::signal` carries, so the manifest names catalogue entries
  /// directly instead of an opaque parallel vocabulary.
  std::string_view key;
  /// <signal @type> — the sign designation ("R1-1"). OpenDRIVE 1.9.0 §14.1
  /// defines @type as a "type identifier according to country code", so a
  /// MUTCD designation is a conforming value.
  std::string_view type;
  /// <signal @subtype>, "-1" when the designation has no variant.
  std::string_view subtype;
  /// <signal @country> — ISO 3166-1 alpha-2 ("US"), or "OpenDRIVE" for the
  /// country-neutral ASAM catalogue signals (§14.1).
  std::string_view country;
  /// props::model() id the placed signal is drawn with.
  std::string_view model_id;
  /// Human-readable name for the Library panel and the properties pane.
  std::string_view label;
  FaceShape shape;
  /// Face extents in meters (§1.4). A zero width means "length fits text"
  /// (D3-1 street-name blades), and such a signal declares no @width.
  double face_width = 0.0;
  double face_height = 0.0;
  /// <signal @dynamic>: a signal head is dynamic, a sign is not.
  bool dynamic = false;
  /// Baked symbol artwork key, "" when the face carries no symbol. Resolved by
  /// the face rasteriser; empty for every entry until the artwork lands.
  std::string_view symbol;
  /// <signal @text> a placement starts with, "" for none.
  std::string_view default_text;
  /// <signal @value> a placement starts with — a speed limit in `unit`.
  std::optional<double> default_value;
  /// <signal @unit>, an e_unitSpeed literal. Required exactly when
  /// default_value is engaged (§14.1); "mph" for the US pack.
  std::string_view unit;
  /// Whether the legend (stop word, speed value, street name) is user-editable
  /// and re-bakes the face.
  bool legend_editable = false;
};

/// Every shipped catalogue entry, in Library order. Valid for the program
/// lifetime (static data).
[[nodiscard]] RM_API std::span<const SignDef> catalog();

/// The entry named by a Library/tool tag, or nullptr.
[[nodiscard]] RM_API const SignDef* find_by_key(std::string_view key);

/// The entry whose designation is EXACTLY (@country, @type, @subtype), or
/// nullptr.
///
/// §14.1 makes @subtype part of a signal's identity — "a signal with the @type
/// and @subtype attributes is only unique in combination with the @country and
/// @countryRevision attributes" — and the shipped pack depends on it: One Way
/// (R6-1) ships a right- and a left-arrow variant differing ONLY in @subtype,
/// drawing DIFFERENT models. test_defaults_registry asserts the triple is
/// unique across the catalogue, so the answer is never ambiguous.
///
/// This is the strict question — "does this build ship this exact
/// designation?" — and the one edit::set_signal_identity asks before it seeds a
/// retyped sign from the table. For drawing or labelling a signal that is
/// already placed, use resolve_designation() instead.
[[nodiscard]] RM_API const SignDef*
find_by_designation(std::string_view country, std::string_view type, std::string_view subtype);

/// The entry that best describes a placed <signal>: its exact designation when
/// this build ships it, otherwise the first entry matching (@country, @type)
/// alone, otherwise nullptr.
///
/// The @subtype-blind fallback is deliberate and must stay: a file may carry a
/// variant of a designation we do know (another country's revision, a pack that
/// is not loaded), and it should still draw and read as that designation rather
/// than collapse to the generic silhouette. Callers must degrade rather than
/// fail on nullptr — a scene authored against another catalogue still has to
/// open.
[[nodiscard]] RM_API const SignDef*
resolve_designation(std::string_view country, std::string_view type, std::string_view subtype);

} // namespace roadmaker::signs
