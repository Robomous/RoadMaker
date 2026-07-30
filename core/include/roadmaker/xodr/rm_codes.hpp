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

#include <array>
#include <string_view>

namespace roadmaker {

/// The XML scope a registered `rm:` userData code is emitted at. One code, one
/// scope: a registered code met at any OTHER scope is not understood there and
/// takes the preserve-and-warn path like a foreign code.
enum class RmCodeScope {
  Road,     ///< child of <road>
  Object,   ///< child of <object> / <bridge>
  Junction, ///< child of <junction>
  Root,     ///< child of <OpenDRIVE>
};

struct RmCode {
  std::string_view code;
  RmCodeScope scope;
};

/// The ADR-0008 `rm:` userData registry — the ONE code-side list of every
/// `<userData code="rm:...">` this writer emits, mirrored verbatim by the
/// registry block in docs/decisions/0008-persistence-layers-asam-first.md
/// (test_rm_registry.cpp keeps the two in sync, and cross-checks that every
/// entry has a parser, a fuzz-corpus sample, and a round-trip test — fmt-s2,
/// #326). Grouped by scope in writer emission order.
///
/// NOT listed here: the `rm:<material-id>` ATTRIBUTE-VALUE namespace
/// (rm:asphalt, rm:paint_white, ...). Those are material ids carried in
/// attributes (lane <material @surface>, <roadMark @material>, rm:junction
/// `mat=` fields, <userData code="rm:surface" material=...>), not userData
/// codes — they never appear as a `code` attribute.
inline constexpr std::array<RmCode, 19> kRmCodes{{
    // <road>
    {"rm:waypoints", RmCodeScope::Road},
    {"rm:aux_boundary", RmCodeScope::Road},
    // <object> / <bridge>
    {"rm:crosswalk", RmCodeScope::Object},
    {"rm:markingCurve", RmCodeScope::Object},
    {"rm:assembly", RmCodeScope::Object},
    {"rm:stencil", RmCodeScope::Object},
    {"rm:stopline", RmCodeScope::Object},
    {"rm:material.bridge_deck", RmCodeScope::Object},
    // <junction>
    {"rm:arms", RmCodeScope::Junction},
    {"rm:corners", RmCodeScope::Junction},
    {"rm:floor", RmCodeScope::Junction},
    {"rm:maneuver", RmCodeScope::Junction},
    {"rm:signal", RmCodeScope::Junction},
    {"rm:signalmount", RmCodeScope::Junction},
    {"rm:phases", RmCodeScope::Junction},
    {"rm:junction", RmCodeScope::Junction},
    {"rm:spans", RmCodeScope::Junction},
    // <OpenDRIVE> root
    {"rm:surface", RmCodeScope::Root},
    {"rm:terrain", RmCodeScope::Root},
}};

/// True iff `code` is a registered RoadMaker userData code (at any scope). An
/// `rm:`-prefixed code that is NOT registered was written by a newer RoadMaker;
/// the parser preserves it verbatim with a structured warning, never drops it
/// (ADR-0008 policy).
[[nodiscard]] constexpr bool is_registered_rm_code(std::string_view code) {
  for (const RmCode& entry : kRmCodes) {
    if (entry.code == code) {
      return true;
    }
  }
  return false;
}

} // namespace roadmaker
