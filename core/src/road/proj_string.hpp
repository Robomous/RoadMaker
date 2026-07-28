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

// PROJ-string tokenising, shared by `road/georeference.cpp` (tmerc_origin) and
// `gis/crs.cpp` (parse_crs).
//
// WHY THIS HEADER EXISTS. p7-s5 wrote a tokeniser inside georeference.cpp to
// answer one question — "is this the tmerc-on-the-origin string we emit, and
// where is its origin?". p7-s2 needs a wider reading of the same syntax. Two
// tokenisers over one syntax is a defect waiting to happen: they would agree on
// the strings anyone tested and diverge on the ones nobody did, and the symptom
// would be a scene that reports one origin and reprojects to another. So there
// is exactly one, here, and `test_gis_crs.cpp` asserts the two callers agree.
//
// PRIVATE to core. Not under core/include — nothing outside the kernel builds
// against this, and the public surface stays `tmerc_origin` and `gis::parse_crs`.

#include <fast_float/fast_float.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace roadmaker::proj_detail {

/// One `+key=value` (or bare `+key`, with an empty value) parameter.
using ProjParam = std::pair<std::string_view, std::string_view>;

/// Locale-independent double parsing, rejecting trailing garbage — the same
/// contract as the reader's `to_double`. std::stod is locale-dependent and must
/// never touch a projection string, which is machine data in every locale.
[[nodiscard]] inline std::optional<double> parse_double(std::string_view text) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  double value{};
  const auto result = fast_float::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

/// Splits a PROJ string into its `+key=value` and bare `+key` parameters.
/// Order-insensitive and whitespace-insensitive by construction, so a string
/// that has been through another tool's formatter still reads.
[[nodiscard]] inline std::vector<ProjParam> proj_parameters(std::string_view projection) {
  std::vector<ProjParam> params;
  std::size_t pos = 0;
  while (pos < projection.size()) {
    const std::size_t start = projection.find('+', pos);
    if (start == std::string_view::npos) {
      break;
    }
    std::size_t end = start + 1;
    while (end < projection.size() && projection[end] != ' ' && projection[end] != '\t' &&
           projection[end] != '\r' && projection[end] != '\n') {
      ++end;
    }
    const std::string_view token = projection.substr(start + 1, end - start - 1);
    const std::size_t eq = token.find('=');
    if (eq == std::string_view::npos) {
      params.emplace_back(token, std::string_view{});
    } else {
      params.emplace_back(token.substr(0, eq), token.substr(eq + 1));
    }
    pos = end;
  }
  return params;
}

/// The value of `key`, or nullopt when it is absent. A key repeated in the
/// string yields its FIRST occurrence, matching how PROJ itself resolves
/// duplicates.
[[nodiscard]] inline std::optional<std::string_view>
proj_value(const std::vector<ProjParam>& params, std::string_view key) {
  for (const auto& [name, value] : params) {
    if (name == key) {
      return value;
    }
  }
  return std::nullopt;
}

/// The numeric value of `key`, or `fallback` when the key is absent. Returns
/// nullopt when the key is present but does not parse — an unreadable number is
/// a defect in the string, never silently the default.
[[nodiscard]] inline std::optional<double>
proj_number(const std::vector<ProjParam>& params, std::string_view key, double fallback) {
  const std::optional<std::string_view> raw = proj_value(params, key);
  if (!raw.has_value()) {
    return fallback;
  }
  return parse_double(*raw);
}

/// True when `key` is absent, or present with a value that parses to `expected`.
/// Absence counts as a match because PROJ defaults `k`, `x_0` and `y_0` to
/// exactly the values this predicate is asked about, so a string that omits
/// them describes the same projection as one that spells them out.
[[nodiscard]] inline bool
proj_number_is(const std::vector<ProjParam>& params, std::string_view key, double expected) {
  const std::optional<std::string_view> raw = proj_value(params, key);
  if (!raw.has_value()) {
    return true;
  }
  const std::optional<double> value = parse_double(*raw);
  return value.has_value() && *value == expected;
}

} // namespace roadmaker::proj_detail
