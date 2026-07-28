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

#include "reference_layers.hpp"

#include "roadmaker/gis/crs.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <system_error>

namespace roadmaker::editor {

namespace {

/// Reads one layer's source file and places it in the scene's frame.
/// Populates `layer.loaded`, `layer.status`, `layer.source_crs` and whichever
/// of `vector`/`raster` applies. Never throws; a failure is a status line.
void load_one(ReferenceLayer& layer,
              const std::filesystem::path& scene_dir,
              const GeoReference& scene_georeference,
              std::vector<Diagnostic>& diagnostics) {
  layer.loaded = false;
  layer.vector = {};
  layer.raster = {};

  const std::filesystem::path source = scene_dir / std::filesystem::path(layer.path);
  const gis::Crs scene = gis::scene_crs(scene_georeference);

  const auto fail = [&](std::string message) {
    layer.status = message;
    diagnostics.push_back(Diagnostic{
        .severity = Severity::Warning, .location = layer.path, .message = std::move(message)});
  };

  if (layer.kind == ReferenceLayerKind::Vector) {
    Expected<gis::GisVectorParseResult> read = gis::load_gis_vector(source);
    if (!read.has_value()) {
      fail(read.error().message);
      return;
    }
    layer.source_crs = read->layer.crs;
    for (Diagnostic& d : read->diagnostics) {
      diagnostics.push_back(std::move(d));
    }
    const gis::Crs from = gis::parse_crs(layer.source_crs);
    // A vector file with no stated CRS is taken as already being in the scene's
    // frame — the reader has already warned about that, and refusing outright
    // would make a plain unprojected sketch unimportable.
    if (!layer.source_crs.empty()) {
      const Expected<gis::CrsTransform> transform = gis::crs_transform(from, scene);
      if (!transform.has_value()) {
        fail(transform.error().message);
        return;
      }
      gis::reproject_vector(read->layer, *transform);
    }
    layer.vector = std::move(read->layer);
    layer.loaded = true;
    layer.status = fmt::format("{} · {} feature{}",
                               layer.source_crs.empty() ? "no stated CRS" : gis::describe_crs(from),
                               layer.vector.features.size(),
                               layer.vector.features.size() == 1 ? "" : "s");
    return;
  }

  Expected<gis::GisRasterParseResult> read = gis::load_gis_raster(source);
  if (!read.has_value()) {
    fail(read.error().message);
    return;
  }
  for (Diagnostic& d : read->diagnostics) {
    diagnostics.push_back(std::move(d));
  }
  layer.source_crs = read->raster.crs;

  if (read->raster.elevation) {
    // Elevation is terrain, not a backdrop, and it goes through the command
    // layer instead. Saying so beats drawing a greyscale height map over the
    // ground and letting the user wonder why it is not affecting anything.
    fail("this is an elevation raster — import it with Edit ▸ Terrain ▸ Import "
         "Elevation Raster, which makes it the scene's terrain");
    return;
  }

  const gis::Crs from = gis::parse_crs(layer.source_crs);
  gis::PlacedRaster placed;
  if (layer.source_crs.empty()) {
    // Already in the scene's frame by assumption; place it as read.
    placed.raster = std::move(read->raster);
    placed.placement = gis::RasterPlacement::Placed;
    const auto& t = placed.raster.transform;
    const double w = t[0] * placed.raster.width;
    const double h = t[3] * placed.raster.height;
    placed.extent = {std::min(t[4], t[4] + w),
                     std::min(t[5], t[5] + h),
                     std::max(t[4], t[4] + w),
                     std::max(t[5], t[5] + h)};
  } else {
    const Expected<gis::CrsTransform> transform = gis::crs_transform(from, scene);
    if (!transform.has_value()) {
      fail(transform.error().message);
      return;
    }
    Expected<gis::PlacedRaster> result =
        gis::reproject_raster(read->raster, *transform, diagnostics);
    if (!result.has_value()) {
      fail(result.error().message);
      return;
    }
    placed = std::move(*result);
  }

  layer.raster = std::move(placed);
  layer.loaded = true;
  layer.status =
      fmt::format("{} · {} · {}×{}",
                  layer.source_crs.empty() ? "no stated CRS" : gis::describe_crs(from),
                  layer.raster.placement == gis::RasterPlacement::Placed ? "placed" : "resampled",
                  layer.raster.raster.width,
                  layer.raster.raster.height);
}

} // namespace

std::string relative_reference(const std::filesystem::path& target,
                               const std::filesystem::path& base) {
  std::error_code ec;
  const std::filesystem::path relative = std::filesystem::relative(target, base, ec);
  if (ec || relative.empty()) {
    return target.generic_string();
  }
  return relative.generic_string();
}

Expected<std::vector<Diagnostic>> ReferenceLayers::add(const std::filesystem::path& source,
                                                       const std::filesystem::path& scene_dir,
                                                       const GeoReference& scene_georeference) {
  ReferenceLayer layer;
  layer.path = relative_reference(source, scene_dir);
  layer.kind =
      gis::is_vector_extension(source) ? ReferenceLayerKind::Vector : ReferenceLayerKind::Raster;
  layer.framed_crs = scene_georeference.projection;

  std::vector<Diagnostic> diagnostics;
  load_one(layer, scene_dir, scene_georeference, diagnostics);
  if (!layer.loaded) {
    // A layer the user just asked for that cannot be placed is an error they
    // must see, not a greyed-out row they might not notice.
    return make_error(ErrorCode::InvalidArgument, layer.status, layer.path);
  }
  layers_.push_back(std::move(layer));
  return diagnostics;
}

void ReferenceLayers::remove(std::size_t index) {
  if (index < layers_.size()) {
    layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void ReferenceLayers::set_visible(std::size_t index, bool visible) {
  if (index < layers_.size()) {
    layers_[index].visible = visible;
  }
}

void ReferenceLayers::clear() {
  layers_.clear();
}

std::vector<Diagnostic> ReferenceLayers::reload(std::vector<ReferenceLayer> persisted,
                                                const std::filesystem::path& scene_dir,
                                                const GeoReference& scene_georeference) {
  std::vector<Diagnostic> diagnostics;
  layers_ = std::move(persisted);
  for (ReferenceLayer& layer : layers_) {
    if (layer.framed_crs != scene_georeference.projection) {
      // Not an error and not a reason to drop the layer: the source file is
      // still the truth, so re-derive the placement in the CURRENT frame. This
      // is where a reference layer differs from the workspace box, which has no
      // source to re-derive from and must therefore be discarded.
      diagnostics.push_back(Diagnostic{
          .severity = Severity::Info,
          .location = layer.path,
          .message = "the scene's georeference changed since this layer was placed, so it was "
                     "re-derived from its source file in the current frame"});
      layer.framed_crs = scene_georeference.projection;
    }
    load_one(layer, scene_dir, scene_georeference, diagnostics);
  }
  return diagnostics;
}

std::vector<Diagnostic> ReferenceLayers::refit(const std::filesystem::path& scene_dir,
                                               const GeoReference& scene_georeference) {
  std::vector<Diagnostic> diagnostics;
  for (ReferenceLayer& layer : layers_) {
    layer.framed_crs = scene_georeference.projection;
    load_one(layer, scene_dir, scene_georeference, diagnostics);
  }
  return diagnostics;
}

std::optional<std::array<double, 4>> ReferenceLayers::bounds() const {
  double min_x = std::numeric_limits<double>::infinity();
  double min_y = std::numeric_limits<double>::infinity();
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = -std::numeric_limits<double>::infinity();
  bool any = false;

  for (const ReferenceLayer& layer : layers_) {
    if (!layer.drawable()) {
      continue;
    }
    const std::array<double, 4> box =
        layer.kind == ReferenceLayerKind::Vector ? layer.vector.bounds : layer.raster.extent;
    if (layer.kind == ReferenceLayerKind::Vector && layer.vector.features.empty()) {
      continue;
    }
    min_x = std::min(min_x, box[0]);
    min_y = std::min(min_y, box[1]);
    max_x = std::max(max_x, box[2]);
    max_y = std::max(max_y, box[3]);
    any = true;
  }

  if (!any) {
    return std::nullopt;
  }
  return std::array<double, 4>{min_x, min_y, max_x, max_y};
}

} // namespace roadmaker::editor
