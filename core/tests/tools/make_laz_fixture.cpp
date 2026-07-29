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

// Writes the committed `amsterdam_tile.laz` fixture from `amsterdam_tile.las`.
//
// WHY THIS IS NOT IN scripts/gen_lidar_fixtures.py, WHERE EVERY OTHER FIXTURE
// LIVES. LAZ is an entropy-coded stream with per-field predictive models; unlike
// the TIFF LZW encoder gen_gis_fixtures.py writes in a page of standard library,
// it cannot be produced without the codec. So this tool exists, is run by hand,
// and its output is COMMITTED — a fixture generated at test time would only ever
// prove that laz-perf agrees with itself.
//
// ★ IT DELIBERATELY DOES NOT LINK roadmaker::core, and reads the handful of
// header fields it needs itself. A fixture generator that used the reader under
// test would bake that reader's mistakes into the fixture, and the comparison
// test would then agree with both of them. The twenty lines below are the price
// of the fixture being independent evidence.
//
// Not a test, and EXCLUDE_FROM_ALL: run once when the fixture needs
// regenerating, never by CI.
//
//   cmake --build --preset dev-macos --target rm_make_laz_fixture
//   ./build/dev-macos/core/tests/rm_make_laz_fixture \
//       core/tests/data/lidar/amsterdam_tile.las \
//       core/tests/data/lidar/amsterdam_tile.laz

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <lazperf/writers.hpp>
#include <string>

namespace {

// LAS is little-endian at every version; assemble rather than memcpy a struct.
std::uint32_t read_u32(const std::string& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24U);
}

std::uint16_t read_u16(const std::string& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(
      static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) |
      (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 8U));
}

double read_f64(const std::string& bytes, std::size_t at) {
  std::uint64_t bits = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8U);
  }
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void write_u32(std::string& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[at + i] = static_cast<char>((value >> (i * 8U)) & 0xFFU);
  }
}

/// The source's `LASF_Projection` records, header and payload, concatenated.
std::string extract_projection_vlrs(const std::string& bytes) {
  constexpr std::size_t kVlrHeaderSize = 54;
  const std::uint16_t header_size = read_u16(bytes, 94);
  const std::uint32_t vlr_count = read_u32(bytes, 100);

  std::string carried;
  std::size_t at = header_size;
  for (std::uint32_t i = 0; i < vlr_count; ++i) {
    if (at + kVlrHeaderSize > bytes.size()) {
      break;
    }
    const std::string user_id(bytes.substr(at + 2, 16).c_str());
    const std::uint16_t payload_size = read_u16(bytes, at + 20);
    if (at + kVlrHeaderSize + payload_size > bytes.size()) {
      break;
    }
    if (user_id == "LASF_Projection") {
      carried += bytes.substr(at, kVlrHeaderSize + payload_size);
    }
    at += kVlrHeaderSize + payload_size;
  }
  return carried;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: rm_make_laz_fixture <input.las> <output.laz>\n");
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  if (bytes.size() < 227 || bytes.compare(0, 4, "LASF") != 0) {
    std::fprintf(stderr, "%s is not a LAS file\n", argv[1]);
    return 1;
  }

  const int minor_version = static_cast<unsigned char>(bytes[25]);
  const std::uint32_t point_data_offset = read_u32(bytes, 96);
  const int point_format = static_cast<unsigned char>(bytes[104]);
  const std::uint16_t record_length = read_u16(bytes, 105);
  std::uint64_t point_count = read_u32(bytes, 107);
  if (minor_version >= 4 && bytes.size() >= 375) {
    std::uint64_t wide = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      wide |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[247 + i])) << (i * 8U);
    }
    if (wide != 0) {
      point_count = wide;
    }
  }

  constexpr std::uint16_t kNaturalLengths[] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
  if (point_format < 0 || point_format > 10) {
    std::fprintf(
        stderr, "%s: point format %d is not one this tool writes\n", argv[1], point_format);
    return 1;
  }

  lazperf::writer::named_file::config config(
      lazperf::vector3(read_f64(bytes, 131), read_f64(bytes, 139), read_f64(bytes, 147)),
      lazperf::vector3(read_f64(bytes, 155), read_f64(bytes, 163), read_f64(bytes, 171)));
  config.pdrf = point_format;
  config.minor_version = minor_version;
  config.extra_bytes = record_length - kNaturalLengths[point_format];

  {
    lazperf::writer::named_file output(argv[2], config);
    for (std::uint64_t i = 0; i < point_count; ++i) {
      output.writePoint(bytes.data() + point_data_offset + (i * record_length));
    }
    output.close();
  }

  // ★ laz-perf's writer EMITS ITS OWN HEADER AND ONLY ITS OWN VLRs — the laszip
  // descriptor and, if asked, an extra-bytes record. Every other variable-length
  // record of the source, INCLUDING ITS COORDINATE REFERENCE SYSTEM, is dropped.
  //
  // A .laz fixture with no CRS would be unlike every .laz in the world, and
  // would leave the reader's VLR walk — which runs BEFORE decompression and has
  // to work on a compressed file — untested. So the source's LASF_Projection
  // record is spliced back in here.
  //
  // Splicing is not just a memcpy: the LAZ body begins with an 8-byte ABSOLUTE
  // offset to the chunk table, so inserting bytes ahead of it invalidates that
  // pointer unless it moves by the same amount.
  const std::string source_vlrs = extract_projection_vlrs(bytes);
  if (source_vlrs.empty()) {
    std::printf("%s: %llu points written (source stated no CRS)\n",
                argv[2],
                static_cast<unsigned long long>(point_count));
    return 0;
  }

  std::ifstream written(argv[2], std::ios::binary);
  std::string laz((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
  written.close();

  const std::uint16_t laz_header_size = read_u16(laz, 94);
  const std::uint32_t laz_point_offset = read_u32(laz, 96);
  const std::uint32_t laz_vlr_count = read_u32(laz, 100);
  const auto shift = static_cast<std::uint32_t>(source_vlrs.size());

  std::string patched = laz.substr(0, laz_header_size) + source_vlrs + laz.substr(laz_header_size);
  write_u32(patched, 96, laz_point_offset + shift);
  write_u32(patched, 100, laz_vlr_count + 1);

  std::uint64_t chunk_table = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    chunk_table |= static_cast<std::uint64_t>(
                       static_cast<unsigned char>(patched[laz_point_offset + shift + i]))
                   << (i * 8U);
  }
  chunk_table += shift;
  for (std::size_t i = 0; i < 8; ++i) {
    patched[laz_point_offset + shift + i] = static_cast<char>((chunk_table >> (i * 8U)) & 0xFFU);
  }

  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  out.write(patched.data(), static_cast<std::streamsize>(patched.size()));
  out.close();

  std::printf("%s: %llu points written, %zu bytes of CRS records carried over\n",
              argv[2],
              static_cast<unsigned long long>(point_count),
              source_vlrs.size());
  return 0;
}
