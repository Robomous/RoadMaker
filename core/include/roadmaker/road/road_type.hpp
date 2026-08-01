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
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/xodr/raw_xml.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace roadmaker {

/// The default speed limit posted for a road type — the `<speed>` child of
/// `<type>` (ASAM OpenDRIVE 1.9.0 §10.4.1, `t_road_type_speed`, multiplicity
/// 0..1).
///
/// **@max is not a number.** Its type `t_maxSpeed` is a union: a numeric value
/// *or* one of exactly two string literals, `"no limit"` and `"undefined"`.
/// Parsing it as a double and writing the double back would turn a German
/// autobahn's `max="no limit"` into `max="0"` on the first save — a silent
/// corruption of the same class as the enum-spelling defect #476 records. So
/// the VERBATIM spelling is the field that round-trips and the parsed number is
/// derived from it, the same split `Object::type_str` / `Object::type` already
/// uses for `e_objectType`.
struct RoadSpeed {
  /// @max exactly as it appeared, and the only field the writer emits. Empty
  /// only on a record whose @max was absent (which the schema forbids, so the
  /// reader warns).
  std::string max_str;

  /// The numeric value of `max_str` in `unit`, or nullopt when @max is one of
  /// the two string literals — or when it is neither a number nor a literal,
  /// which is a foreign file's problem to have and not ours to guess at.
  /// **Derived. Never the round-trip source.**
  std::optional<double> max;

  /// @unit — `e_unitSpeed`, one of "km/h", "m/s", "mph". Empty when absent;
  /// §10.4.1 says m/s is then implied, but an absent attribute is written back
  /// absent rather than materialised.
  std::string unit;

  /// Unmodeled attributes, preserved verbatim so round-trip loses nothing.
  RawXml extras;

  friend bool operator==(const RoadSpeed&, const RoadSpeed&) = default;
};

/// One `<type>` record: the road's main purpose over an s-range, and the speed
/// limit that goes with it (§10.4, `t_road_type`, multiplicity 0..*).
///
/// A record is valid from its @s until the next record or the end of the road.
/// Records are held ascending by @s
/// (`asam.net:xodr:1.4.0:road.type.elem_asc_order`).
struct RoadTypeRecord {
  /// @s — start position along the reference line [m], `t_grEqZero`, required.
  double s = 0.0;

  /// @type — `e_roadType`, required. Kept as the literal spelling rather than
  /// an enum so a value outside the 13 the standard defines survives a round
  /// trip instead of being flattened to "unknown". The enumeration is
  /// **identical in 1.8.1 (A.6.3, Table 188) and 1.9.0 (A.6.2, Table 194)** —
  /// same 13 literals, same order — so nothing here is revision-conditional.
  std::string type;

  /// @country — optional ISO 3166-1 **alpha-2** code
  /// (`asam.net:xodr:1.7.0:road.type.only_alpha_2_country_codes`; alpha-3 is
  /// forbidden because only alpha-2 supports state identifiers). Empty when
  /// absent.
  std::string country;

  /// The `<speed>` child, when the record carries one.
  std::optional<RoadSpeed> speed;

  /// Unmodeled attributes and unmodeled children, preserved verbatim.
  RawXml extras;

  friend bool operator==(const RoadTypeRecord&, const RoadTypeRecord&) = default;
};

/// The single `<type>` record a road of `road_class` should carry (#454).
///
/// `defaults::road_type_name` supplies the `@type` spelling from
/// `docs/domain/realism_defaults.md` §1.7, which is where the class→type
/// binding is governed; this wraps it into the record the model stores so the
/// creation path and the restyle path cannot drift apart.
///
/// **`carry_over` is what keeps this from destroying data.** A road being
/// restyled already has type records, and those records may carry a `<speed>`,
/// an `@country` and preserved extras. §1.7 is explicit that there is
/// deliberately **no per-class default speed** — "a speed limit is a fact about
/// a particular road, not about its class" — so a restyle changes the `@type`
/// and keeps everything else the old record said. Pass the record covering
/// s = 0, or nullptr when the road has none.
///
/// The result always sits at `s = 0`: a class is a property of the whole road,
/// and every caller applies a uniform cross section.
[[nodiscard]] RM_API RoadTypeRecord road_type_for_class(defaults::RoadClass road_class,
                                                        const RoadTypeRecord* carry_over = nullptr);

/// The 13 `e_roadType` literals (§16 A.6.2). Exposed so a validator, a
/// mapping table or a UI can ask whether a spelling is one the standard
/// defines — without anyone re-typing the list.
[[nodiscard]] RM_API bool is_known_road_type(std::string_view type);

/// The 3 `e_unitSpeed` literals (§16 A.1.5): "km/h", "m/s", "mph".
[[nodiscard]] RM_API bool is_known_speed_unit(std::string_view unit);

/// The two string forms `t_maxSpeed` permits in place of a number.
inline constexpr std::string_view kMaxSpeedNoLimit = "no limit";
inline constexpr std::string_view kMaxSpeedUndefined = "undefined";

} // namespace roadmaker
