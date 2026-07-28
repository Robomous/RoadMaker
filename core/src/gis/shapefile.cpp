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

// ESRI Shapefile reader (ESRI Shapefile Technical Description 07/98).
//
// A shapefile is a SET of files sharing a stem: `.shp` holds the geometry,
// `.shx` its index, `.dbf` the attributes, `.prj` the coordinate system. Only
// `.shp` is required here — the index is redundant when reading sequentially,
// and attributes are cosmetic — but a missing `.prj` matters, because then the
// file does not say where on the earth it is.
//
// The format's one genuine trap: the 100-byte header is BIG-endian for its
// first two fields and LITTLE-endian for the rest, and each record's own header
// is big-endian while its contents are little-endian. Mixing those up produces
// plausible-looking garbage rather than an obvious failure, which is why the
// two readers below are named for their byte order at every call site.

#include "roadmaker/gis/layer.hpp"

#include <fmt/format.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gis_common.hpp"

namespace roadmaker::gis {

namespace {

constexpr std::size_t kHeaderSize = 100;
constexpr std::int32_t kFileCode = 9994;

std::int32_t read_be_i32(const char* p) {
  const auto* b = reinterpret_cast<const std::uint8_t*>(p);
  return static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
      (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]));
}

std::int32_t read_le_i32(const char* p) {
  const auto* b = reinterpret_cast<const std::uint8_t*>(p);
  return static_cast<std::int32_t>(
      static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
      (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24));
}

/// Little-endian IEEE-754 double. Read through memcpy rather than a cast: the
/// bytes in a file have no alignment guarantee, and type-punning through a
/// pointer cast is undefined behaviour the sanitizer build would (rightly)
/// flag.
double read_le_f64(const char* p) {
  std::uint64_t bits = 0;
  const auto* b = reinterpret_cast<const std::uint8_t*>(p);
  for (int i = 7; i >= 0; --i) {
    bits = (bits << 8) | static_cast<std::uint64_t>(b[i]);
  }
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/// Shape types we read. The Z and M variants carry extra trailing arrays that
/// we skip: a reference layer is drawn in plan, so the elevation a shapefile
/// happens to carry is not what positions it.
enum ShapeType : std::int32_t {
  kNull = 0,
  kPoint = 1,
  kPolyLine = 3,
  kPolygon = 5,
  kMultiPoint = 8,
  kPointZ = 11,
  kPolyLineZ = 13,
  kPolygonZ = 15,
  kMultiPointZ = 18,
  kPointM = 21,
  kPolyLineM = 23,
  kPolygonM = 25,
  kMultiPointM = 28,
};

std::string shape_type_name(std::int32_t type) {
  switch (type) {
  case kNull:
    return "Null";
  case kPoint:
  case kPointZ:
  case kPointM:
    return "Point";
  case kPolyLine:
  case kPolyLineZ:
  case kPolyLineM:
    return "PolyLine";
  case kPolygon:
  case kPolygonZ:
  case kPolygonM:
    return "Polygon";
  case kMultiPoint:
  case kMultiPointZ:
  case kMultiPointM:
    return "MultiPoint";
  default:
    return fmt::format("type {}", type);
  }
}

/// Base shape ignoring the Z/M decoration, so one switch handles all three
/// spellings of each geometry.
std::int32_t base_shape_type(std::int32_t type) {
  switch (type) {
  case kPointZ:
  case kPointM:
    return kPoint;
  case kPolyLineZ:
  case kPolyLineM:
    return kPolyLine;
  case kPolygonZ:
  case kPolygonM:
    return kPolygon;
  case kMultiPointZ:
  case kMultiPointM:
    return kMultiPoint;
  default:
    return type;
  }
}

/// Reads a PolyLine/Polygon record body: bounding box, part count, point
/// count, the part start indices, then the points.
bool read_parts_and_points(std::string_view body,
                           GisFeature& feature,
                           std::vector<Diagnostic>& diagnostics,
                           std::string_view location) {
  // 32 bytes box + 4 numParts + 4 numPoints
  constexpr std::size_t kPrefix = 40;
  if (body.size() < kPrefix) {
    return false;
  }
  const std::int32_t num_parts = read_le_i32(body.data() + 32);
  const std::int32_t num_points = read_le_i32(body.data() + 36);
  if (num_parts < 0 || num_points < 0) {
    return false;
  }
  const auto parts = static_cast<std::size_t>(num_parts);
  const auto points = static_cast<std::size_t>(num_points);
  const std::size_t needed = kPrefix + (parts * 4) + (points * 16);
  if (body.size() < needed) {
    return false;
  }

  std::vector<std::size_t> starts;
  starts.reserve(parts);
  for (std::size_t i = 0; i < parts; ++i) {
    const std::int32_t start = read_le_i32(body.data() + kPrefix + (i * 4));
    if (start < 0 || static_cast<std::size_t>(start) > points) {
      return false;
    }
    starts.push_back(static_cast<std::size_t>(start));
  }

  const char* point_base = body.data() + kPrefix + (parts * 4);
  for (std::size_t i = 0; i < points; ++i) {
    feature.vertices.push_back(
        {read_le_f64(point_base + (i * 16)), read_le_f64(point_base + (i * 16) + 8)});
  }

  // A record with no parts but some points is malformed; treat the whole run as
  // one part rather than dropping the geometry.
  if (starts.empty()) {
    if (!feature.vertices.empty()) {
      diagnostics.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = std::string(location),
                     .message = "a record declared no parts; its points were read as one run"});
      starts.push_back(0);
    } else {
      return false;
    }
  }
  feature.ring_starts = std::move(starts);
  return true;
}

/// The first character field of a `.dbf` whose name looks like a label, per
/// record. dBASE III format: a 32-byte header, then 32 bytes per field
/// descriptor, terminated by 0x0D; records are fixed width with a leading
/// deletion flag.
///
/// This is deliberately shallow. Attribute tables are cosmetic here, so a `.dbf`
/// this cannot read costs a layer nothing but its labels.
std::vector<std::string> read_dbf_names(std::string_view dbf) {
  constexpr std::size_t kDbfHeader = 32;
  constexpr std::size_t kFieldDescriptor = 32;
  if (dbf.size() < kDbfHeader) {
    return {};
  }
  const std::int32_t record_count = read_le_i32(dbf.data() + 4);
  const auto read_le_u16 = [&dbf](std::size_t at) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(dbf[at])) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(dbf[at + 1]) << 8U));
  };
  const std::uint16_t header_size = read_le_u16(8);
  const std::uint16_t record_size = read_le_u16(10);
  if (record_count <= 0 || header_size < kDbfHeader || record_size == 0 ||
      dbf.size() < header_size) {
    return {};
  }

  std::size_t offset = 1; // skip the leading deletion flag
  std::size_t name_offset = 0;
  std::size_t name_width = 0;
  bool found = false;
  for (std::size_t at = kDbfHeader; at + kFieldDescriptor <= header_size; at += kFieldDescriptor) {
    if (dbf[at] == '\r') {
      break;
    }
    std::string field(dbf.substr(at, 11));
    if (const std::size_t nul = field.find('\0'); nul != std::string::npos) {
      field.resize(nul);
    }
    const char type = dbf[at + 11];
    const auto width = static_cast<std::size_t>(static_cast<std::uint8_t>(dbf[at + 16]));
    if (!found && type == 'C') {
      std::string upper;
      upper.reserve(field.size());
      for (const char c : field) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      }
      if (upper == "NAME" || upper == "REF" || upper == "LABEL" || upper == "ID") {
        name_offset = offset;
        name_width = width;
        found = true;
      }
    }
    offset += width;
  }
  if (!found) {
    return {};
  }

  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(record_count));
  for (std::size_t r = 0; r < static_cast<std::size_t>(record_count); ++r) {
    const std::size_t at = header_size + (r * record_size) + name_offset;
    if (at + name_width > dbf.size()) {
      break;
    }
    std::string value(dbf.substr(at, name_width));
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
      value.pop_back();
    }
    names.push_back(std::move(value));
  }
  return names;
}

} // namespace

Expected<GisVectorParseResult> load_shapefile(const std::filesystem::path& path) {
  const Expected<std::string> bytes = read_file_bytes(path);
  if (!bytes.has_value()) {
    return make_error(bytes.error().code, bytes.error().message, bytes.error().context);
  }
  const std::string& shp = *bytes;
  const std::string source_name = path.filename().string();

  if (shp.size() < kHeaderSize) {
    return make_error(ErrorCode::InvalidDocument,
                      "too short to be a shapefile (the header alone is 100 bytes)",
                      source_name);
  }
  if (read_be_i32(shp.data()) != kFileCode) {
    return make_error(ErrorCode::InvalidDocument,
                      "not a shapefile: the header's file code is not 9994",
                      source_name);
  }

  GisVectorParseResult result;

  // `.prj` is where a shapefile says which CRS its coordinates are in. Its
  // absence is not fatal — the caller decides what an unstated CRS means — but
  // it must be said out loud, because the alternative is an import that lands
  // somewhere arbitrary with no explanation.
  result.layer.crs = read_sibling_prj(path);
  if (result.layer.crs.empty()) {
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = source_name,
        .message = "no .prj file beside the .shp, so the file does not state its coordinate "
                   "reference system; its coordinates are read as already being in the scene's "
                   "frame"});
  }

  std::vector<std::string> names;
  if (const std::optional<std::filesystem::path> dbf = sibling(path, ".dbf"); dbf.has_value()) {
    if (const Expected<std::string> dbf_bytes = read_file_bytes(*dbf); dbf_bytes.has_value()) {
      names = read_dbf_names(*dbf_bytes);
    }
  }

  std::size_t offset = kHeaderSize;
  std::size_t record_index = 0;
  while (offset + 8 <= shp.size()) {
    // Record header: big-endian record number and content length IN 16-BIT
    // WORDS — the single most commonly mis-implemented field in the format.
    const std::int32_t length_words = read_be_i32(shp.data() + offset + 4);
    offset += 8;
    if (length_words <= 0) {
      break;
    }
    const std::size_t content_size = static_cast<std::size_t>(length_words) * 2;
    if (offset + content_size > shp.size()) {
      result.diagnostics.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = fmt::format("{}: record {}", source_name, record_index),
                     .message = "the file ends mid-record; everything before it was read"});
      break;
    }

    const std::string_view body(shp.data() + offset, content_size);
    offset += content_size;
    const std::size_t this_index = record_index;
    ++record_index;

    if (body.size() < 4) {
      continue;
    }
    const std::int32_t declared = read_le_i32(body.data());
    const std::int32_t type = base_shape_type(declared);
    const std::string location = fmt::format("{}: record {}", source_name, this_index);

    GisFeature feature;
    if (this_index < names.size()) {
      feature.name = names[this_index];
    }

    if (type == kNull) {
      continue; // a legal "this record has no shape"; nothing to draw, nothing to warn about
    }

    if (type == kPoint) {
      if (body.size() < 20) {
        continue;
      }
      feature.geometry = GisFeature::Geometry::Point;
      feature.ring_starts.push_back(0);
      feature.vertices.push_back({read_le_f64(body.data() + 4), read_le_f64(body.data() + 12)});
      result.layer.features.push_back(std::move(feature));
      continue;
    }

    if (type == kMultiPoint) {
      constexpr std::size_t kPrefix = 36; // 4 type + 32 box
      if (body.size() < kPrefix + 4) {
        continue;
      }
      const std::int32_t count = read_le_i32(body.data() + kPrefix);
      if (count < 0 || body.size() < kPrefix + 4 + (static_cast<std::size_t>(count) * 16)) {
        continue;
      }
      for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        GisFeature point;
        point.geometry = GisFeature::Geometry::Point;
        point.name = feature.name;
        point.ring_starts.push_back(0);
        point.vertices.push_back({read_le_f64(body.data() + kPrefix + 4 + (i * 16)),
                                  read_le_f64(body.data() + kPrefix + 4 + (i * 16) + 8)});
        result.layer.features.push_back(std::move(point));
      }
      continue;
    }

    if (type == kPolyLine || type == kPolygon) {
      feature.geometry =
          type == kPolygon ? GisFeature::Geometry::Polygon : GisFeature::Geometry::Line;
      if (!read_parts_and_points(body.substr(4), feature, result.diagnostics, location)) {
        result.diagnostics.push_back(
            Diagnostic{.severity = Severity::Warning,
                       .location = location,
                       .message = fmt::format("a {} record was malformed and was skipped",
                                              shape_type_name(declared))});
        continue;
      }
      result.layer.features.push_back(std::move(feature));
      continue;
    }

    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = location,
                   .message = fmt::format("shape {} is not a geometry this build reads and was "
                                          "skipped",
                                          shape_type_name(declared))});
  }

  if (const Expected<void> capped = enforce_vertex_budget(result.layer, source_name);
      !capped.has_value()) {
    return make_error(capped.error().code, capped.error().message, capped.error().context);
  }

  recompute_bounds(result.layer);
  return result;
}

} // namespace roadmaker::gis
