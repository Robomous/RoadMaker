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

// ASPRS LAS reader. Layouts come from the LAS 1.4 R15 specification; every
// offset below is quoted from it rather than inferred, because a mistake in an
// offset table reads plausible numbers out of the wrong bytes and looks like
// data rather than like a bug (ADR-0011 names this as the cost of not
// delegating).
//
// EVERYTHING ON DISK IS LITTLE-ENDIAN, at every version, so the readers below
// assemble values byte by byte instead of memcpy-ing a struct. That is also what
// makes the reader indifferent to the host's alignment rules and to any padding
// a compiler might insert.

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../gis/gis_common.hpp"
#include "lidar_common.hpp"

namespace roadmaker::lidar {

namespace {

constexpr std::string_view kSignature = "LASF";
constexpr std::string_view kProjectionUserId = "LASF_Projection";

/// `LASF_Projection` record ids, per LAS 1.4 §CRS VLRs.
constexpr std::uint16_t kRecordGeoKeyDirectory = 34735;
constexpr std::uint16_t kRecordMathTransformWkt = 2111;
constexpr std::uint16_t kRecordCoordinateSystemWkt = 2112;

/// Public header block sizes by version. 1.0 through 1.2 share one layout; 1.3
/// appends the waveform pointer; 1.4 appends the EVLR fields and the 64-bit
/// point counts.
constexpr std::uint16_t kHeaderSize12 = 227;
constexpr std::uint16_t kHeaderSize13 = 235;
constexpr std::uint16_t kHeaderSize14 = 375;

/// Global Encoding bit 4: "if this bit is set, the CRS for the file will be
/// located in the WKT (Extended) Variable Length Records".
constexpr std::uint16_t kGlobalEncodingWkt = 1U << 4U;

/// The high bit of the on-disk point-format byte marks a LAZ-compressed stream.
/// It is laszip's convention rather than an ASPRS one, which is why it is
/// stripped here and carried as a separate flag.
constexpr std::uint8_t kLazCompressionBit = 0x80;

/// A fixed-width character field, trimmed at the first NUL. LAS pads these with
/// NULs rather than terminating them, so a naive std::string would carry the
/// padding into every comparison.
[[nodiscard]] std::string
read_fixed_string(std::string_view bytes, std::size_t at, std::size_t width) {
  const std::string_view field = bytes.substr(at, width);
  const std::size_t end = field.find('\0');
  return std::string(end == std::string_view::npos ? field : field.substr(0, end));
}

} // namespace

bool format_has_waveform(std::uint8_t point_format) {
  return point_format == 4 || point_format == 5 || point_format == 9 || point_format == 10;
}

std::uint16_t point_record_length(std::uint8_t point_format) {
  // LAS 1.4 R15, point data record formats 0-10. Formats 0-5 are the legacy
  // layout; 6-10 widened the return-number field, promoted classification to a
  // whole byte and moved the flags into their own.
  constexpr std::array<std::uint16_t, 11> kLengths{20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
  return point_format < kLengths.size() ? kLengths[point_format] : 0;
}

ClassificationLayout classification_layout(std::uint8_t point_format) {
  // Formats 0-5: byte 15, with the class in bits 0-4 and synthetic/key-point/
  // withheld above it. Reading that byte unmasked reports class 34 for a
  // withheld ground point — which is not a class at all, and would quietly
  // exclude exactly the returns a ground fit wants.
  // Formats 6-10: byte 16, whole, with the flags moved to byte 15.
  if (point_format >= 6) {
    return ClassificationLayout{.offset = 16, .masked = false};
  }
  return ClassificationLayout{.offset = 15, .masked = true};
}

Expected<LasHeader> parse_las_header(std::string_view bytes, std::string_view source_name) {
  if (bytes.size() < kHeaderSize12) {
    return make_error(ErrorCode::InvalidDocument,
                      "this file is too short to hold a LAS public header block",
                      std::string(source_name));
  }
  if (bytes.substr(0, kSignature.size()) != kSignature) {
    return make_error(ErrorCode::InvalidDocument,
                      "this file does not start with the LAS signature \"LASF\"",
                      std::string(source_name));
  }

  LasHeader header;
  header.global_encoding = read_u16(bytes, 6);
  header.version_major = byte_at(bytes, 24);
  header.version_minor = byte_at(bytes, 25);

  if (header.version_major != 1) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("this file declares LAS {}.{}; this build reads LAS 1.0 through 1.4",
                    header.version_major,
                    header.version_minor),
        std::string(source_name));
  }
  if (header.version_minor > 4) {
    // Named rather than lumped in with "unsupported": 1.5 is a real revision
    // with a longer header and a WKT-only CRS rule, and #490 is where it lands.
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("this file is LAS 1.{}; this build reads LAS 1.0 through 1.4, and 1.5 support "
                    "is tracked as issue #490",
                    header.version_minor),
        std::string(source_name));
  }

  header.header_size = read_u16(bytes, 94);
  header.point_data_offset = read_u32(bytes, 96);
  header.vlr_count = read_u32(bytes, 100);

  const std::uint8_t raw_format = byte_at(bytes, 104);
  header.compressed = (raw_format & kLazCompressionBit) != 0;
  header.point_format = static_cast<std::uint8_t>(raw_format & ~kLazCompressionBit);
  header.point_record_length = read_u16(bytes, 105);
  header.point_count = read_u32(bytes, 107);

  for (std::size_t axis = 0; axis < 3; ++axis) {
    header.scale[axis] = read_f64(bytes, 131 + (axis * 8));
    header.offset[axis] = read_f64(bytes, 155 + (axis * 8));
  }
  // The min/max block interleaves as max-x, min-x, max-y, min-y, max-z, min-z.
  for (std::size_t axis = 0; axis < 3; ++axis) {
    header.max[axis] = read_f64(bytes, 179 + (axis * 16));
    header.min[axis] = read_f64(bytes, 187 + (axis * 16));
  }

  const std::uint16_t expected_header_size = header.version_minor >= 4   ? kHeaderSize14
                                             : header.version_minor == 3 ? kHeaderSize13
                                                                         : kHeaderSize12;
  if (header.header_size < expected_header_size) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("this file declares LAS 1.{} but a {}-byte header, where the specification "
                    "requires {}",
                    header.version_minor,
                    header.header_size,
                    expected_header_size),
        std::string(source_name));
  }

  if (header.version_minor >= 4 && bytes.size() >= kHeaderSize14) {
    header.evlr_count = read_u32(bytes, 243);
    const std::uint64_t wide_count = read_u64(bytes, 247);
    // 1.4 requires the legacy field to be zero whenever the count exceeds
    // UINT32_MAX or a 6-10 point format is used, so the wide field wins
    // whenever it is populated — but a 1.4 writer that filled only the legacy
    // one is common enough in the wild that falling back matters.
    if (wide_count != 0) {
      header.point_count = wide_count;
    }
  }

  if (header.point_record_length < point_record_length(header.point_format)) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("this file declares point data record format {} with {}-byte records, but "
                    "that format needs at least {} bytes",
                    header.point_format,
                    header.point_record_length,
                    point_record_length(header.point_format)),
        std::string(source_name));
  }
  if (point_record_length(header.point_format) == 0) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("this file uses point data record format {}; this build reads formats 0 "
                    "through 10",
                    header.point_format),
        std::string(source_name));
  }

  return header;
}

std::vector<LasVlr> parse_las_vlrs(std::string_view bytes,
                                   const LasHeader& header,
                                   std::vector<Diagnostic>& diagnostics,
                                   std::string_view source_name) {
  // VLR header: reserved u16, user id char[16], record id u16, record length
  // after header u16, description char[32] — 54 bytes, then the payload.
  constexpr std::size_t kVlrHeaderSize = 54;

  std::vector<LasVlr> records;
  std::size_t at = header.header_size;

  for (std::uint32_t i = 0; i < header.vlr_count; ++i) {
    if (at + kVlrHeaderSize > bytes.size() || at + kVlrHeaderSize > header.point_data_offset) {
      diagnostics.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = std::string(source_name),
                     .message = fmt::format("the variable-length record list ends after {} of the "
                                            "{} records the header declares",
                                            i,
                                            header.vlr_count)});
      break;
    }

    LasVlr record;
    record.user_id = read_fixed_string(bytes, at + 2, 16);
    record.record_id = read_u16(bytes, at + 18);
    const std::uint16_t payload_size = read_u16(bytes, at + 20);
    const std::size_t payload_at = at + kVlrHeaderSize;
    if (payload_at + payload_size > bytes.size()) {
      diagnostics.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = std::string(source_name),
          .message = fmt::format("variable-length record \"{}\":{} claims {} bytes past the end of "
                                 "the file and was skipped",
                                 record.user_id,
                                 record.record_id,
                                 payload_size)});
      break;
    }
    record.payload = bytes.substr(payload_at, payload_size);
    records.push_back(std::move(record));
    at = payload_at + payload_size;
  }

  return records;
}

std::string crs_from_vlrs(const std::vector<LasVlr>& vlrs,
                          const LasHeader& header,
                          std::vector<Diagnostic>& diagnostics,
                          std::string_view source_name) {
  const LasVlr* wkt = nullptr;
  const LasVlr* geokeys = nullptr;
  for (const LasVlr& record : vlrs) {
    if (record.user_id != kProjectionUserId) {
      continue;
    }
    if (record.record_id == kRecordCoordinateSystemWkt) {
      wkt = &record;
    } else if (record.record_id == kRecordGeoKeyDirectory) {
      geokeys = &record;
    }
    // 2111 is the OGC Math Transform WKT, which describes a transform rather
    // than a coordinate system. Named so the reader is not silently ignoring a
    // record it was handed.
    else if (record.record_id == kRecordMathTransformWkt) {
      diagnostics.push_back(
          Diagnostic{.severity = Severity::Info,
                     .location = std::string(source_name),
                     .message = "the file carries an OGC Math Transform record, which describes a "
                                "transform rather than a coordinate system, and was not read"});
    }
  }

  const bool wkt_declared = (header.global_encoding & kGlobalEncodingWkt) != 0;

  // Point formats 6-10 are WKT-only by specification: a file using one without
  // the WKT bit is an error per LAS 1.4. It is read anyway when GeoKeys are
  // present — refusing a file whose CRS is legible would help nobody — but the
  // discrepancy is reported rather than swallowed.
  if (header.point_format >= 6 && !wkt_declared && geokeys != nullptr) {
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = std::string(source_name),
        .message = fmt::format("point data record format {} requires the coordinate system to be "
                               "stated as WKT, but this file states it as GeoTIFF keys; the keys "
                               "were read",
                               header.point_format)});
  }

  if (wkt != nullptr) {
    std::string text(wkt->payload);
    // The payload is NUL-terminated by convention and NUL-padded in practice.
    const std::size_t end = text.find('\0');
    if (end != std::string::npos) {
      text.resize(end);
    }
    if (!text.empty()) {
      return text;
    }
  }

  if (geokeys != nullptr) {
    // The directory is a flat array of little-endian uint16s. Decoded here
    // rather than aliased, so the reader behaves identically on a big-endian
    // host and never depends on the payload's alignment.
    const std::size_t count = geokeys->payload.size() / 2;
    std::vector<std::uint16_t> keys(count);
    for (std::size_t i = 0; i < count; ++i) {
      keys[i] = read_u16(geokeys->payload, i * 2);
    }
    return gis::crs_from_geokey_directory(
        std::span<const std::uint16_t>(keys), diagnostics, source_name);
  }

  if (wkt_declared) {
    diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = std::string(source_name),
                   .message = "the file says its coordinate system is stated as WKT, but carries "
                              "no WKT record"});
  }
  return {};
}

} // namespace roadmaker::lidar
