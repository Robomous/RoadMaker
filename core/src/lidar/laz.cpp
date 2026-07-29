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

// The LAZ half of the reader: laz-perf turns the compressed stream back into
// the point records the rest of `roadmaker::lidar` already knows how to walk.
//
// THIS FILE IS THE ONLY PLACE laz-perf IS NAMED. The public header, the VLRs
// and the record layouts are RoadMaker's (ADR-0011), so `.las` and `.laz`
// converge on one code path the instant the bytes are readable, and a laz-perf
// that ever became unavailable would cost `.laz` support and nothing else.
//
// LAZ is an entropy-coded stream, so it cannot be seeked into by point index the
// way an uncompressed body can. Decoding is therefore sequential and every point
// passes through the decoder — but only the kept ones are materialised, so a
// decimated read holds one record at a time rather than the whole tile.

#include <fmt/format.h>

// <algorithm> is for std::any_of and std::min below. It arrives transitively on
// macOS through libc++, so a clean local build is NOT evidence it is present —
// Linux clang caught this one.
#include <algorithm>
#include <cstddef>
#include <exception>
#include <lazperf/readers.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lidar_common.hpp"

namespace roadmaker::lidar {

Expected<std::size_t> stream_laz_points(std::string_view bytes,
                                        const LasHeader& header,
                                        const std::vector<LasVlr>& vlrs,
                                        std::size_t stride,
                                        std::vector<Diagnostic>& diagnostics,
                                        std::string_view source_name,
                                        const PointSink& sink) {
  const bool has_laszip_vlr = std::any_of(vlrs.begin(), vlrs.end(), [](const LasVlr& record) {
    return record.user_id == "laszip encoded" && record.record_id == 22204;
  });
  if (!has_laszip_vlr) {
    return make_error(
        ErrorCode::InvalidDocument,
        "this file's header says its points are LAZ-compressed but it carries no \"laszip "
        "encoded\" record describing how, so the stream cannot be decoded",
        std::string(source_name));
  }

  // laz-perf owns the buffer only for reading, but its mem_file takes a
  // non-const pointer. The copy is deliberate rather than a const_cast: it keeps
  // the promise that a reader never writes to the caller's bytes, and the buffer
  // is the file we already hold either way.
  std::string owned(bytes);

  std::size_t delivered = 0;
  try {
    lazperf::reader::mem_file file(owned.data(), owned.size());

    const std::size_t length = header.point_record_length;
    const std::size_t total =
        std::min<std::size_t>(header.point_count, static_cast<std::size_t>(file.pointCount()));
    if (static_cast<std::size_t>(file.pointCount()) != header.point_count) {
      diagnostics.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = std::string(source_name),
          .message = fmt::format("the header declares {} points and the compressed stream holds "
                                 "{} — the smaller count was read",
                                 header.point_count,
                                 file.pointCount())});
    }

    std::vector<char> record(length);
    for (std::size_t i = 0; i < total; ++i) {
      file.readPoint(record.data());
      if (i % stride != 0) {
        continue;
      }
      sink(std::string_view(record.data(), length));
      ++delivered;
    }
  } catch (const std::exception& error) {
    // laz-perf reports through exceptions; the kernel's API does not throw
    // across its boundary, so this is where they stop. The decoder's own words
    // are carried through — they name the chunk or the field that failed, which
    // is more use than "could not decompress".
    return make_error(ErrorCode::InvalidDocument,
                      fmt::format("the LAZ stream could not be decoded: {}", error.what()),
                      std::string(source_name));
  }

  return delivered;
}

} // namespace roadmaker::lidar
