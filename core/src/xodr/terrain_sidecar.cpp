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

#include "roadmaker/xodr/terrain_sidecar.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <system_error>

namespace roadmaker {

namespace {

/// Shortest round-trip spelling, matching the .xodr writer's `num()` so both
/// files agree on how a double looks and neither drifts on re-save.
std::string num(double value) {
  std::string text = fmt::format("{}", value);
  return text == "-0" ? "0" : text;
}

/// Case-insensitive compare for the header keys — the format's own writers
/// disagree on capitalisation (`NODATA_value` vs `nodata_value`).
bool key_is(std::string_view token, std::string_view key) {
  return std::ranges::equal(token, key, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

/// Splits on any run of ASCII whitespace, keeping the token's line number so a
/// diagnostic can point at it.
struct Token {
  std::string_view text;
  std::size_t line = 1;
};

std::vector<Token> tokenize(std::string_view text) {
  std::vector<Token> tokens;
  std::size_t line = 1;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      if (text[i] == '\n') {
        ++line;
      }
      ++i;
    }
    const std::size_t start = i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
      ++i;
    }
    if (i > start) {
      tokens.push_back({text.substr(start, i - start), line});
    }
  }
  return tokens;
}

/// Strict double parse — the whole token must be consumed. Mirrors the xodr
/// reader's to_double so a value the .xodr would reject is rejected here too.
bool to_double(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
  double value = 0.0;
  const auto* const end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

} // namespace

bool is_safe_sidecar_reference(std::string_view reference) {
  if (reference.empty()) {
    return false;
  }
  // A sidecar sits NEXT TO the .xodr. Anything that could escape that directory
  // — an absolute path, a separator, a drive letter, a ".." component — is data
  // from a foreign file, not a name we may hand to the filesystem.
  if (reference.find('/') != std::string_view::npos ||
      reference.find('\\') != std::string_view::npos ||
      reference.find(':') != std::string_view::npos) {
    return false;
  }
  return reference != "." && reference != "..";
}

Expected<std::string> write_terrain_asc(const HeightField& field) {
  if (field.empty()) {
    return make_error(
        ErrorCode::InvalidArgument, "cannot serialize an empty height field", "terrain sidecar");
  }
  if (!(field.spacing > 0.0) || field.heights.size() != field.cols * field.rows) {
    return make_error(ErrorCode::InvalidArgument, "malformed height field", "terrain sidecar");
  }

  std::string out;
  out.reserve(field.heights.size() * 4);
  out += fmt::format("ncols         {}\n", field.cols);
  out += fmt::format("nrows         {}\n", field.rows);
  out += fmt::format("xllcorner     {}\n", num(field.origin_x));
  out += fmt::format("yllcorner     {}\n", num(field.origin_y));
  out += fmt::format("cellsize      {}\n", num(field.spacing));
  out += fmt::format("NODATA_value  {}\n", num(kAscNoData));

  // The format's rows run NORTH first; HeightField stores them south first.
  // This loop and its twin in the parser are the only two places that flip.
  for (std::size_t row = field.rows; row-- > 0;) {
    for (std::size_t col = 0; col < field.cols; ++col) {
      if (col > 0) {
        out += ' ';
      }
      out += num(field.heights[(row * field.cols) + col]);
    }
    out += '\n';
  }
  return out;
}

Expected<TerrainParseResult> parse_terrain_asc(std::string_view text,
                                               std::string_view source_name) {
  const std::vector<Token> tokens = tokenize(text);

  // Header: six key/value pairs, in any order, before the first value row.
  std::size_t cursor = 0;
  double ncols = 0.0;
  double nrows = 0.0;
  double xll = 0.0;
  double yll = 0.0;
  double cellsize = 0.0;
  double nodata = kAscNoData;
  bool have_cols = false;
  bool have_rows = false;
  bool have_cell = false;
  TerrainParseResult result;

  while (cursor + 1 < tokens.size()) {
    const std::string_view key = tokens[cursor].text;
    double value = 0.0;
    const bool numeric = to_double(tokens[cursor + 1].text, value);
    if (key_is(key, "ncols")) {
      ncols = value;
      have_cols = numeric;
    } else if (key_is(key, "nrows")) {
      nrows = value;
      have_rows = numeric;
    } else if (key_is(key, "xllcorner") || key_is(key, "xllcenter")) {
      xll = value;
    } else if (key_is(key, "yllcorner") || key_is(key, "yllcenter")) {
      yll = value;
    } else if (key_is(key, "cellsize")) {
      cellsize = value;
      have_cell = numeric;
    } else if (key_is(key, "nodata_value")) {
      nodata = value;
    } else {
      break; // not a header key — the value rows start here
    }
    if (!numeric) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("header key '{}' has a non-numeric value", key),
                        fmt::format("{}:{}", source_name, tokens[cursor + 1].line));
    }
    cursor += 2;
  }

  if (!have_cols || !have_rows || !have_cell) {
    return make_error(ErrorCode::InvalidArgument,
                      "missing one of the required header keys ncols/nrows/cellsize",
                      std::string{source_name});
  }
  if (!(cellsize > 0.0)) {
    return make_error(
        ErrorCode::InvalidArgument, "cellsize must be positive", std::string{source_name});
  }
  if (!(ncols >= 1.0) || !(nrows >= 1.0) || ncols != std::floor(ncols) ||
      nrows != std::floor(nrows)) {
    return make_error(ErrorCode::InvalidArgument,
                      "ncols/nrows must be positive whole numbers",
                      std::string{source_name});
  }

  HeightField field;
  field.origin_x = xll;
  field.origin_y = yll;
  field.spacing = cellsize;
  field.cols = static_cast<std::size_t>(ncols);
  field.rows = static_cast<std::size_t>(nrows);

  const std::size_t expected = field.cols * field.rows;
  if (tokens.size() - cursor < expected) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("expected {} values, found {}", expected, tokens.size() - cursor),
                      std::string{source_name});
  }

  // Read north-first rows into the south-first store — the twin of the writer's
  // flip. A NODATA post becomes 0 (flat) with a diagnostic; the alternative,
  // propagating a sentinel into the geometry, would put a -9999 m cliff in the
  // scene.
  std::size_t nodata_count = 0;
  field.heights.assign(expected, 0.0);
  for (std::size_t row = field.rows; row-- > 0;) {
    for (std::size_t col = 0; col < field.cols; ++col) {
      const Token& token = tokens[cursor++];
      double value = 0.0;
      if (!to_double(token.text, value)) {
        return make_error(ErrorCode::InvalidArgument,
                          fmt::format("value '{}' is not a number", token.text),
                          fmt::format("{}:{}", source_name, token.line));
      }
      if (value == nodata) {
        ++nodata_count;
        value = 0.0;
      }
      field.heights[(row * field.cols) + col] = value;
    }
  }

  if (nodata_count > 0) {
    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = std::string{source_name},
                   .message = fmt::format("{} NODATA cell(s) read as height 0", nodata_count),
                   .rule_id = {}});
  }
  if (tokens.size() - cursor > 0) {
    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = std::string{source_name},
                   .message = fmt::format("{} trailing value(s) after the grid were ignored",
                                          tokens.size() - cursor),
                   .rule_id = {}});
  }
  result.field = std::move(field);
  return result;
}

Expected<void> save_terrain_asc(const HeightField& field, const std::filesystem::path& path) {
  auto text = write_terrain_asc(field);
  if (!text) {
    return tl::unexpected<Error>(text.error());
  }
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open file for writing", path.string());
  }
  stream << *text;
  if (!stream.good()) {
    return make_error(ErrorCode::IoFailure, "write failed", path.string());
  }
  return {};
}

Expected<TerrainParseResult> load_terrain_asc(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return make_error(ErrorCode::FileNotFound, "terrain sidecar not found", path.string());
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open terrain sidecar", path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = std::move(buffer).str();
  return parse_terrain_asc(text, path.string());
}

} // namespace roadmaker
