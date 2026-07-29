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

// Shared internals of the lidar reader. PRIVATE to core — the public surface is
// `lidar/point_cloud.hpp`.

#include "roadmaker/error.hpp"
#include "roadmaker/lidar/point_cloud.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker::lidar {

// --- Little-endian readers -------------------------------------------------
//
// EVERYTHING IN A LAS FILE IS LITTLE-ENDIAN, at every version, so values are
// assembled byte by byte rather than memcpy-ed out of a struct. That is what
// makes the reader indifferent to the host's byte order, to its alignment rules
// and to any padding a compiler might insert — and these live in the shared
// header because the public header block and the point records are read by
// different translation units and must agree exactly.

[[nodiscard]] inline std::uint8_t byte_at(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint8_t>(bytes[at]);
}

[[nodiscard]] inline std::uint16_t read_u16(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint16_t>(static_cast<unsigned>(byte_at(bytes, at)) |
                                    (static_cast<unsigned>(byte_at(bytes, at + 1)) << 8U));
}

[[nodiscard]] inline std::uint32_t read_u32(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint32_t>(byte_at(bytes, at)) |
         (static_cast<std::uint32_t>(byte_at(bytes, at + 1)) << 8U) |
         (static_cast<std::uint32_t>(byte_at(bytes, at + 2)) << 16U) |
         (static_cast<std::uint32_t>(byte_at(bytes, at + 3)) << 24U);
}

[[nodiscard]] inline std::uint64_t read_u64(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint64_t>(read_u32(bytes, at)) |
         (static_cast<std::uint64_t>(read_u32(bytes, at + 4)) << 32U);
}

[[nodiscard]] inline std::int32_t read_i32(std::string_view bytes, std::size_t at) {
  return static_cast<std::int32_t>(read_u32(bytes, at));
}

/// IEEE-754 doubles are stored little-endian in the file. Assembling the bit
/// pattern and memcpy-ing it is the only portable spelling — reinterpreting a
/// pointer is undefined behaviour, and the sanitizer job says so.
[[nodiscard]] inline double read_f64(std::string_view bytes, std::size_t at) {
  const std::uint64_t bits = read_u64(bytes, at);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/// The parsed LAS public header block, in the fields this reader actually uses.
///
/// Everything here is little-endian on disk by specification, at every version.
struct LasHeader {
  std::uint8_t version_major = 0;
  std::uint8_t version_minor = 0;
  std::uint16_t global_encoding = 0;
  std::uint16_t header_size = 0;
  std::uint32_t point_data_offset = 0;
  std::uint32_t vlr_count = 0;
  std::uint8_t point_format = 0; ///< already stripped of the LAZ compression bit
  bool compressed = false;       ///< the high bit of the on-disk point-format byte
  std::uint16_t point_record_length = 0;
  std::uint64_t point_count = 0; ///< the legacy 32-bit field, or 1.4's 64-bit one
  std::array<double, 3> scale{1.0, 1.0, 1.0};
  std::array<double, 3> offset{0.0, 0.0, 0.0};
  std::array<double, 3> min{0.0, 0.0, 0.0};
  std::array<double, 3> max{0.0, 0.0, 0.0};
  std::uint32_t evlr_count = 0;
};

/// True when the point format carries waveform packet references, which this
/// reader skips rather than parses.
[[nodiscard]] bool format_has_waveform(std::uint8_t point_format);

/// Bytes a point record of this format occupies, or 0 for a format this reader
/// does not know. A file may declare a LONGER record (extra bytes are legal and
/// common); it may never declare a shorter one.
[[nodiscard]] std::uint16_t point_record_length(std::uint8_t point_format);

/// Byte offset of the classification field within a point record, and whether it
/// needs the 5-bit mask.
///
/// Formats 0-5 pack the class into bits 0-4 of byte 15, with synthetic,
/// key-point and withheld in the top three; formats 6-10 promoted it to the
/// whole of byte 16 and moved the flags into byte 15. Reading byte 15 unmasked
/// on an old file would report class 34 for a withheld ground point.
struct ClassificationLayout {
  std::size_t offset = 15;
  bool masked = true;
};

[[nodiscard]] ClassificationLayout classification_layout(std::uint8_t point_format);

/// Parses the public header block out of the first bytes of a file.
[[nodiscard]] Expected<LasHeader> parse_las_header(std::string_view bytes,
                                                   std::string_view source_name);

/// One variable-length record's payload, keyed by the pair that identifies it.
struct LasVlr {
  std::string user_id;
  std::uint16_t record_id = 0;
  std::string_view payload;
};

/// Walks the VLRs between the header and the point data.
///
/// Malformed or truncated records end the walk rather than failing the read: a
/// file whose point data is sound should still import, minus whatever the
/// damaged record would have told us.
[[nodiscard]] std::vector<LasVlr> parse_las_vlrs(std::string_view bytes,
                                                 const LasHeader& header,
                                                 std::vector<Diagnostic>& diagnostics,
                                                 std::string_view source_name);

/// The CRS those VLRs describe, as a string `gis::parse_crs` understands, or
/// empty when the file states none.
[[nodiscard]] std::string crs_from_vlrs(const std::vector<LasVlr>& vlrs,
                                        const LasHeader& header,
                                        std::vector<Diagnostic>& diagnostics,
                                        std::string_view source_name);

/// Receives one point record at a time, exactly `point_record_length` bytes
/// long. The two streamers below differ only in where those bytes come from,
/// which is the whole of what LAZ changes.
using PointSink = std::function<void(std::string_view record)>;

/// Walks every `stride`-th point record of an uncompressed `.las` body.
/// Returns how many records were delivered.
[[nodiscard]] Expected<std::size_t> stream_las_points(std::string_view bytes,
                                                      const LasHeader& header,
                                                      std::size_t stride,
                                                      std::vector<Diagnostic>& diagnostics,
                                                      std::string_view source_name,
                                                      const PointSink& sink);

/// The same, decoding a LAZ body through laz-perf.
///
/// laz-perf is used for exactly this and nothing else: the header, the VLRs and
/// the record layouts are ours, so `.las` and `.laz` converge on one code path
/// the moment the bytes are readable (ADR-0011). Decoding is sequential and
/// only the kept records are materialised, so a decimated read of a large tile
/// never holds the whole decompressed body.
[[nodiscard]] Expected<std::size_t> stream_laz_points(std::string_view bytes,
                                                      const LasHeader& header,
                                                      const std::vector<LasVlr>& vlrs,
                                                      std::size_t stride,
                                                      std::vector<Diagnostic>& diagnostics,
                                                      std::string_view source_name,
                                                      const PointSink& sink);

} // namespace roadmaker::lidar
