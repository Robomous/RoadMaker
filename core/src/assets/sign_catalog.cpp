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

#include "roadmaker/assets/sign_catalog.hpp"

#include "roadmaker/road/defaults.hpp"

#include <algorithm>
#include <array>

namespace roadmaker::signs {

namespace {

// The shipped catalogue. Every face extent is a roadmaker::defaults constant
// (spec §1.4) — a literal here would defeat the divergence gate.
//
// model_id names the mesh a placement draws with, and symbol the artwork its
// face composites (assets/signs/us/<symbol>.svg, embedded by
// scripts/gen_sign_symbols.py). resolve_designation() callers must still tolerate
// a model id that props::model() does not know: a catalogue can outrun the
// asset bundle, and a sign whose mesh is missing has to degrade rather than
// vanish.
constexpr std::array<SignDef, 15> k_catalog{{
    {.key = "us.signal_head",
     .type = "1000001", // ASAM catalogue traffic light, §14.1
     .subtype = "-1",
     .country = "OpenDRIVE",
     .model_id = "signal_light",
     .label = "Traffic light",
     .shape = FaceShape::None,
     .face_width = 0.0,
     .face_height = 0.0,
     .dynamic = true,
     .symbol = "",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r1_1",
     .type = "R1-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r1_1",
     .label = "Stop sign (R1-1)",
     .shape = FaceShape::Octagon,
     .face_width = defaults::kSignStopFace,
     .face_height = defaults::kSignStopFace,
     .dynamic = false,
     .symbol = "",
     .default_text = "STOP",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = true},
    {.key = "us.r1_2",
     .type = "R1-2",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r1_2",
     .label = "Yield sign (R1-2)",
     .shape = FaceShape::TriangleDown,
     .face_width = defaults::kSignYieldFace,
     .face_height = defaults::kSignYieldFace,
     .dynamic = false,
     .symbol = "",
     .default_text = "YIELD",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r2_1",
     .type = "R2-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r2_1",
     .label = "Speed limit (R2-1)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignSpeedLimitWidth,
     .face_height = defaults::kSignSpeedLimitHeight,
     .dynamic = false,
     .symbol = "",
     .default_text = "",
     // §1.4: the face displays mph regardless of the display-unit toggle, so
     // the authored value IS in mph and needs no conversion to render.
     .default_value = 25.0,
     .unit = "mph",
     .legend_editable = true},
    {.key = "us.r5_1",
     .type = "R5-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r5_1",
     .label = "Do not enter (R5-1)",
     .shape = FaceShape::Disc,
     .face_width = defaults::kSignDoNotEnterFace,
     .face_height = defaults::kSignDoNotEnterFace,
     .dynamic = false,
     .symbol = "R5-1",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r6_1_right",
     .type = "R6-1",
     .subtype = "R", // §1.4: arrow direction is the variant
     .country = "US",
     .model_id = "sign_us_r6_1_right",
     .label = "One way, right (R6-1)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignOneWayWidth,
     .face_height = defaults::kSignOneWayHeight,
     .dynamic = false,
     .symbol = "R6-1-right",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r6_1_left",
     .type = "R6-1",
     .subtype = "L",
     .country = "US",
     .model_id = "sign_us_r6_1_left",
     .label = "One way, left (R6-1)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignOneWayWidth,
     .face_height = defaults::kSignOneWayHeight,
     .dynamic = false,
     .symbol = "R6-1-left",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r3_1",
     .type = "R3-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r3_1",
     .label = "No right turn (R3-1)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignTurnRestrictionFace,
     .face_height = defaults::kSignTurnRestrictionFace,
     .dynamic = false,
     .symbol = "R3-1",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r3_2",
     .type = "R3-2",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r3_2",
     .label = "No left turn (R3-2)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignTurnRestrictionFace,
     .face_height = defaults::kSignTurnRestrictionFace,
     .dynamic = false,
     .symbol = "R3-2",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.r4_7",
     .type = "R4-7",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_r4_7",
     .label = "Keep right (R4-7)",
     .shape = FaceShape::Rectangle,
     .face_width = defaults::kSignKeepRightWidth,
     .face_height = defaults::kSignKeepRightHeight,
     .dynamic = false,
     .symbol = "R4-7",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.w1_2",
     .type = "W1-2",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_w1_2",
     .label = "Curve ahead (W1-2)",
     .shape = FaceShape::Diamond,
     .face_width = defaults::kSignWarningFace,
     .face_height = defaults::kSignWarningFace,
     .dynamic = false,
     .symbol = "W1-2",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.w3_1",
     .type = "W3-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_w3_1",
     .label = "Stop ahead (W3-1)",
     .shape = FaceShape::Diamond,
     .face_width = defaults::kSignWarningFace,
     .face_height = defaults::kSignWarningFace,
     .dynamic = false,
     .symbol = "W3-1",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.w11_2",
     .type = "W11-2",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_w11_2",
     .label = "Pedestrian crossing (W11-2)",
     .shape = FaceShape::Diamond,
     .face_width = defaults::kSignWarningFace,
     .face_height = defaults::kSignWarningFace,
     .dynamic = false,
     .symbol = "W11-2",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.s1_1",
     .type = "S1-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_s1_1",
     .label = "School (S1-1)",
     .shape = FaceShape::Pentagon,
     .face_width = defaults::kSignSchoolFace,
     .face_height = defaults::kSignSchoolFace,
     .dynamic = false,
     .symbol = "S1-1",
     .default_text = "",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = false},
    {.key = "us.d3_1",
     .type = "D3-1",
     .subtype = "-1",
     .country = "US",
     .model_id = "sign_us_d3_1",
     .label = "Street name (D3-1)",
     .shape = FaceShape::Rectangle,
     // §1.4: "length fits text" — a blade declares no @width.
     .face_width = 0.0,
     .face_height = defaults::kSignStreetNameHeight,
     .dynamic = false,
     .symbol = "",
     .default_text = "MAIN ST",
     .default_value = std::nullopt,
     .unit = "",
     .legend_editable = true},
}};

} // namespace

std::span<const SignDef> catalog() {
  return {k_catalog.data(), k_catalog.size()};
}

const SignDef* find_by_key(std::string_view key) {
  const auto found = std::find_if(
      k_catalog.begin(), k_catalog.end(), [key](const SignDef& def) { return def.key == key; });
  return found == k_catalog.end() ? nullptr : &*found;
}

const SignDef*
find_by_designation(std::string_view country, std::string_view type, std::string_view subtype) {
  const auto found = std::find_if(
      k_catalog.begin(), k_catalog.end(), [country, type, subtype](const SignDef& def) {
        return def.country == country && def.type == type && def.subtype == subtype;
      });
  return found == k_catalog.end() ? nullptr : &*found;
}

const SignDef*
resolve_designation(std::string_view country, std::string_view type, std::string_view subtype) {
  if (const SignDef* exact = find_by_designation(country, type, subtype); exact != nullptr) {
    return exact;
  }
  // A @subtype this build does not ship still names a designation we know.
  const auto found =
      std::find_if(k_catalog.begin(), k_catalog.end(), [country, type](const SignDef& def) {
        return def.country == country && def.type == type;
      });
  return found == k_catalog.end() ? nullptr : &*found;
}

} // namespace roadmaker::signs
