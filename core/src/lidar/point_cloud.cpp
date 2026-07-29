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

// The public lidar entry points: read a tile, move it into the scene's frame.

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../gis/gis_common.hpp"
#include "lidar_common.hpp"

namespace roadmaker::lidar {

namespace {

/// Fills `origin`, `bounds` and `xyz` from absolute coordinates.
///
/// The origin is the centre of the bounding box rather than its corner, which
/// halves the largest offset a float has to carry and therefore the worst-case
/// quantisation. Bounds are taken from the points actually KEPT, not from the
/// header's declared extent: the header describes the whole file, and a
/// decimated cloud that reported the file's box would draw a selection outline
/// around points it does not contain.
void finalize_cloud(PointCloud& cloud, const std::vector<double>& absolute) {
  const std::size_t count = absolute.size() / 3;
  cloud.xyz.clear();
  cloud.bounds = {};
  cloud.origin = {};
  if (count == 0) {
    return;
  }

  std::array<double, 3> low{std::numeric_limits<double>::max(),
                            std::numeric_limits<double>::max(),
                            std::numeric_limits<double>::max()};
  std::array<double, 3> high{std::numeric_limits<double>::lowest(),
                             std::numeric_limits<double>::lowest(),
                             std::numeric_limits<double>::lowest()};
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double value = absolute[(i * 3) + axis];
      low[axis] = std::min(low[axis], value);
      high[axis] = std::max(high[axis], value);
    }
  }

  for (std::size_t axis = 0; axis < 3; ++axis) {
    cloud.bounds[axis] = low[axis];
    cloud.bounds[axis + 3] = high[axis];
    cloud.origin[axis] = (low[axis] + high[axis]) * 0.5;
  }

  cloud.xyz.resize(count * 3);
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      cloud.xyz[(i * 3) + axis] = static_cast<float>(absolute[(i * 3) + axis] - cloud.origin[axis]);
    }
  }
}

} // namespace

Expected<std::size_t> stream_las_points(std::string_view bytes,
                                        const LasHeader& header,
                                        std::size_t stride,
                                        std::vector<Diagnostic>& diagnostics,
                                        std::string_view source_name,
                                        const PointSink& sink) {
  const std::size_t length = header.point_record_length;
  const std::size_t start = header.point_data_offset;
  if (start > bytes.size()) {
    return make_error(ErrorCode::InvalidDocument,
                      "the header points at point data past the end of the file",
                      std::string(source_name));
  }

  const std::size_t available = (bytes.size() - start) / length;
  if (available < header.point_count) {
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = std::string(source_name),
        .message = fmt::format("the header declares {} points but the file holds {} — the tile "
                               "looks truncated, and only the points present were read",
                               header.point_count,
                               available)});
  }
  const std::size_t total = std::min<std::size_t>(header.point_count, available);

  std::size_t delivered = 0;
  for (std::size_t i = 0; i < total; i += stride) {
    sink(bytes.substr(start + (i * length), length));
    ++delivered;
  }
  return delivered;
}

Expected<PointCloudParseResult> load_point_cloud(const std::filesystem::path& path,
                                                 const LidarReadOptions& options) {
  const std::string source_name = path.filename().string();

  if (!is_point_cloud_extension(path)) {
    return make_error(
        ErrorCode::InvalidArgument,
        fmt::format("\"{}\" is not a point-cloud format this build reads — supported: ASPRS LAS "
                    "(.las) and LAZ (.laz)",
                    gis::lower_extension(path)),
        source_name);
  }

  const Expected<std::string> bytes = gis::read_file_bytes(path);
  if (!bytes.has_value()) {
    return make_error(bytes.error().code, bytes.error().message, bytes.error().context);
  }

  PointCloudParseResult result;
  const Expected<LasHeader> header = parse_las_header(*bytes, source_name);
  if (!header.has_value()) {
    return make_error(header.error().code, header.error().message, header.error().context);
  }

  const std::vector<LasVlr> vlrs = parse_las_vlrs(*bytes, *header, result.diagnostics, source_name);
  result.cloud.crs = crs_from_vlrs(vlrs, *header, result.diagnostics, source_name);
  result.cloud.source_count = header->point_count;

  if (format_has_waveform(header->point_format)) {
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning,
        .location = source_name,
        .message = fmt::format("point data record format {} carries waveform packet references, "
                               "which are not read — the returns themselves are unaffected",
                               header->point_format)});
  }
  if (header->evlr_count > 0) {
    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Info,
                   .location = source_name,
                   .message = fmt::format("{} extended variable-length record(s) were not read",
                                          header->evlr_count)});
  }

  // ★ THE STRIDE IS DECIDED FROM THE HEADER, BEFORE A SINGLE POINT IS TOUCHED.
  // A tile too large to hold is also too large to read and then thin, so
  // decimating after the fact would have exactly the failure mode the budget
  // exists to prevent.
  const std::size_t max_points = std::max<std::size_t>(options.max_points, 1);
  std::size_t stride = 1;
  if (header->point_count > max_points) {
    stride = ((header->point_count + max_points - 1) / max_points);
  }
  result.cloud.stride = stride;

  const ClassificationLayout classification = classification_layout(header->point_format);
  const bool has_classification = header->point_record_length > classification.offset;

  std::vector<double> absolute;
  absolute.reserve(3 * std::min<std::size_t>(max_points, header->point_count));
  result.cloud.classification.clear();
  if (has_classification) {
    result.cloud.classification.reserve(std::min<std::size_t>(max_points, header->point_count));
  }

  const PointSink sink = [&](std::string_view record) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const std::int32_t raw = read_i32(record, axis * 4);
      absolute.push_back((static_cast<double>(raw) * header->scale[axis]) + header->offset[axis]);
    }
    if (has_classification) {
      const auto byte = static_cast<std::uint8_t>(record[classification.offset]);
      result.cloud.classification.push_back(
          classification.masked ? static_cast<std::uint8_t>(byte & 0x1FU) : byte);
    }
  };

  const Expected<std::size_t> delivered =
      header->compressed
          ? stream_laz_points(*bytes, *header, vlrs, stride, result.diagnostics, source_name, sink)
          : stream_las_points(*bytes, *header, stride, result.diagnostics, source_name, sink);
  if (!delivered.has_value()) {
    return make_error(delivered.error().code, delivered.error().message, delivered.error().context);
  }

  finalize_cloud(result.cloud, absolute);

  if (result.cloud.empty()) {
    return make_error(ErrorCode::InvalidDocument, "this tile contains no points", source_name);
  }

  if (stride > 1) {
    // Said out loud, always. A cloud silently reduced to a twelfth of itself
    // looks like a sparse survey rather than a decimated one, and every
    // judgement a user makes about coverage would be wrong.
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Info,
        .location = source_name,
        .message = fmt::format("kept 1 point in {} — {} of the tile's {} returns, so that it fits "
                               "the {} the viewport holds",
                               stride,
                               result.cloud.size(),
                               header->point_count,
                               kMaxCloudPoints)});
  }
  if (result.cloud.crs.empty()) {
    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = source_name,
                   .message = "this tile states no coordinate reference system, so it is placed as "
                              "though its coordinates were already in the scene's frame"});
  }

  return result;
}

void reproject_point_cloud(PointCloud& cloud, const gis::CrsTransform& transform) {
  if (cloud.empty()) {
    return;
  }

  // ★ REPROJECTION RUNS ON ABSOLUTE DOUBLES AND THE RESULT IS RE-OFFSET.
  // Transforming the float offsets would put the cloud back into the precision
  // hole the offset representation exists to avoid, one step before the values
  // reach the renderer.
  const std::size_t count = cloud.size();
  std::vector<double> absolute(count * 3);
  for (std::size_t i = 0; i < count; ++i) {
    const std::array<double, 3> world = cloud.point(i);
    const std::array<double, 2> mapped = transform.apply(world[0], world[1]);
    absolute[(i * 3)] = mapped[0];
    absolute[(i * 3) + 1] = mapped[1];
    // Z is untouched: every CRS in the supported family shares one ellipsoid
    // and none of them is a vertical datum, so a height means the same thing on
    // both sides of the transform. Claiming otherwise would be a datum shift,
    // which ADR-0010 refuses to perform or imply.
    absolute[(i * 3) + 2] = world[2];
  }

  finalize_cloud(cloud, absolute);
}

bool is_point_cloud_extension(const std::filesystem::path& path) {
  const std::string extension = gis::lower_extension(path);
  return extension == ".las" || extension == ".laz";
}

} // namespace roadmaker::lidar
