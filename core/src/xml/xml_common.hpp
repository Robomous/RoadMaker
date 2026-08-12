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

// Internal (non-installed) pugixml scalar helpers shared by both persistence
// layers, xodr/ and osc/. Each existed twice, per format, each copy carrying a
// comment defending the duplication against a divergence that never happened
// (#563). Split them again the day a policy actually differs.
//
// NOT shared, deliberately: `append_fragment`. The two formats pass different
// pugixml parse flags (OpenSCENARIO adds `parse_fragment`) — a real divergence.

#include <fmt/format.h>
#include <pugixml.hpp>

#include <fast_float/fast_float.h>

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace roadmaker::xml_common {

/// Locale-independent double parsing; rejects trailing garbage (whitespace is
/// tolerated). `std::stod` is locale-dependent — never use it for ASAM IO.
///
/// A value this rejects is never dropped: both readers fall through to the
/// preserve-the-spelling path. That is the correct outcome for OpenSCENARIO's
/// `$parameter` expressions (§9), whose meaning is only known at runtime.
[[nodiscard]] inline std::optional<double> to_double(std::string_view text) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  double value{};
  const auto result = fast_float::from_chars(first, last, value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  for (const char* p = result.ptr; p != last; ++p) {
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      return std::nullopt;
    }
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

/// Shortest-precision round-trippable formatting; locale-independent.
///
/// The "-0" normalization is load-bearing: without it a negative zero reaches
/// the file and no round trip normalizes it away.
[[nodiscard]] inline std::string num(double value) {
  std::string text = fmt::format("{}", value);
  return text == "-0" ? "0" : text;
}

inline void set_num(pugi::xml_node node, const char* name, double value) {
  node.append_attribute(name).set_value(num(value).c_str());
}

inline void
set_optional_num(pugi::xml_node node, const char* name, const std::optional<double>& value) {
  if (value.has_value()) {
    set_num(node, name, *value);
  }
}

/// Serializes a node as a self-contained XML fragment, for the verbatim
/// preservation tier (roadmaker/xodr/raw_xml.hpp).
///
/// `pugi::format_raw` drops whatever indentation the source document happened
/// to carry, which is why a preserved fragment comes back re-canonicalized
/// rather than byte-identical — fmt-s2's caveat (#326) originates here.
[[nodiscard]] inline std::string node_to_string(const pugi::xml_node& node) {
  std::ostringstream out;
  node.print(out, "", pugi::format_raw);
  return out.str();
}

} // namespace roadmaker::xml_common
