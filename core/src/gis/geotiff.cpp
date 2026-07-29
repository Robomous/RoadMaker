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

// GeoTIFF reader: libtiff decodes the pixels, this file reads the geo tags.
//
// WHY NOT libgeotiff. It is the obvious companion library, and it is skipped on
// purpose: its useful half (`GTIFGetDefn`, turning a GeoKey set into a CRS)
// resolves EPSG codes out of PROJ's database, which is exactly the dependency
// ADR-0010 declines. The GeoKeys themselves are a flat array of 4×uint16
// records — reading the handful we support is less code than integrating a
// library to read them for us, and it keeps the "bounded family" boundary in
// one place instead of two.
//
// COMPRESSION. libtiff is built with only its internal codecs (see
// cmake/deps.cmake): no compression, PackBits and LZW. A Deflate- or
// JPEG-compressed GeoTIFF is refused BY NAME, citing #484 — which is a decision
// about dependencies, not an accident of the decoder.

#include "roadmaker/gis/crs.hpp"
#include "roadmaker/gis/layer.hpp"

#include <fmt/format.h>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "../road/proj_string.hpp"
#include "gis_common.hpp"

namespace roadmaker::gis {

namespace {

// GeoTIFF 1.1 tags (OGC 19-008r4 §7).
constexpr std::uint32_t kTagModelPixelScale = 33550;
constexpr std::uint32_t kTagModelTiepoint = 33922;
constexpr std::uint32_t kTagModelTransformation = 34264;
constexpr std::uint32_t kTagGeoKeyDirectory = 34735;
constexpr std::uint32_t kTagGdalNoData = 42113;

// GeoDoubleParams (34736) and GeoAsciiParams (34737) hold the values of keys
// whose `tiff_tag_location` points at them. No key in the supported family
// does — EPSG codes are plain shorts stored inline — so they are named here and
// deliberately not read.

// The GeoKey ids themselves live in gis_common.cpp beside the directory reader,
// because LAS carries the same directory in a VLR with no TIFF involved.

/// RAII for the libtiff handle. libtiff is a C library and every early return
/// below would otherwise leak it — the sanitizer job would catch that, but only
/// on the paths a test happens to walk.
class TiffHandle {
public:
  explicit TiffHandle(TIFF* tif) : tif_(tif) {}

  TiffHandle(const TiffHandle&) = delete;
  TiffHandle& operator=(const TiffHandle&) = delete;
  TiffHandle(TiffHandle&&) = delete;
  TiffHandle& operator=(TiffHandle&&) = delete;

  ~TiffHandle() {
    if (tif_ != nullptr) {
      TIFFClose(tif_);
    }
  }

  [[nodiscard]] TIFF* get() const { return tif_; }

private:
  TIFF* tif_ = nullptr;
};

std::string compression_name(std::uint16_t compression) {
  switch (compression) {
  case COMPRESSION_NONE:
    return "uncompressed";
  case COMPRESSION_LZW:
    return "LZW";
  case COMPRESSION_PACKBITS:
    return "PackBits";
  case COMPRESSION_ADOBE_DEFLATE:
  case COMPRESSION_DEFLATE:
    return "Deflate";
  case COMPRESSION_JPEG:
    return "JPEG";
  case COMPRESSION_OJPEG:
    return "old-style JPEG";
  case COMPRESSION_LZMA:
    return "LZMA";
  case COMPRESSION_ZSTD:
    return "ZSTD";
  case COMPRESSION_WEBP:
    return "WebP";
  case COMPRESSION_CCITTFAX3:
  case COMPRESSION_CCITTFAX4:
    return "CCITT fax";
  default:
    return fmt::format("compression tag {}", compression);
  }
}

/// The CRS a GeoTIFF's GeoKeys describe, as a string `parse_crs` understands.
///
/// The GeoKey directory is a flat uint16 array: a 4-value header
/// (version, revision, minor, key count) then 4 values per key
/// (id, tiff_tag_location, count, value_or_offset). When tiff_tag_location is
/// 0, `value_or_offset` IS the value — which is the only case a supported CRS
/// ever takes, since EPSG codes are plain shorts.
std::string
crs_from_geokeys(TIFF* tif, std::vector<Diagnostic>& diagnostics, std::string_view source_name) {
  // NOTE THE COUNT TYPE. None of the GeoTIFF tags are registered fields in
  // libtiff, so it treats them as anonymous ones — and `_TIFFCreateAnonField`
  // gives those `TIFF_VARIABLE2`, which means TIFFGetField's variadic contract
  // is (uint32_t* count, T** values). A `uint16_t` count here is a two-byte
  // slot taking a four-byte write, and the single-argument form (the shape a
  // REGISTERED tag uses) writes the count where the value pointer belongs.
  // Both corrupt the stack and both appear to work until the stack layout
  // shifts — which is how this arrived: as a segfault three tests away.
  std::uint32_t count = 0;
  std::uint16_t* keys = nullptr;
  if (TIFFGetField(tif, kTagGeoKeyDirectory, &count, &keys) != 1 || keys == nullptr || count < 4) {
    return {};
  }

  // The directory itself is read by `crs_from_geokey_directory`, which LAS
  // shares (p7-s3, #243). All this function owns is getting the array out of
  // libtiff, which is the part that is TIFF-specific — and the part that has a
  // scar on it.
  return crs_from_geokey_directory(
      std::span<const std::uint16_t>(keys, count), diagnostics, source_name);
}

/// Pixel→CRS affine from the geo tags, in world-file order.
///
/// Two spellings exist and both are in the wild: ModelTransformation is a full
/// 4×4, while the far more common ModelPixelScale + ModelTiepoint pair
/// describes an axis-aligned map. Rotation only ever appears in the former.
std::optional<std::array<double, 6>> read_transform(TIFF* tif) {
  // uint32_t counts throughout — see the note in crs_from_geokeys.
  std::uint32_t count = 0;
  double* values = nullptr;

  if (TIFFGetField(tif, kTagModelTransformation, &count, &values) == 1 && values != nullptr &&
      count >= 16) {
    // Row-major 4×4 mapping (col, row, z, 1) -> (x, y, z, 1).
    return std::array<double, 6>{values[0], values[4], values[1], values[5], values[3], values[7]};
  }

  std::uint32_t scale_count = 0;
  double* scale = nullptr;
  std::uint32_t tie_count = 0;
  double* tie = nullptr;
  if (TIFFGetField(tif, kTagModelPixelScale, &scale_count, &scale) == 1 && scale != nullptr &&
      scale_count >= 2 && TIFFGetField(tif, kTagModelTiepoint, &tie_count, &tie) == 1 &&
      tie != nullptr && tie_count >= 6) {
    // Tiepoint: (i, j, k) in raster space maps to (x, y, z) in model space.
    // Pixel scale's y is POSITIVE in the tag and the row axis runs the other
    // way, hence the negation — get this wrong and the image imports mirrored.
    const double x0 = tie[3] - (tie[0] * scale[0]);
    const double y0 = tie[4] + (tie[1] * scale[1]);
    return std::array<double, 6>{scale[0], 0.0, 0.0, -scale[1], x0, y0};
  }

  return std::nullopt;
}

std::optional<double> read_nodata(TIFF* tif) {
  // GDAL_NODATA is an ASCII tag and, like the geo tags, unregistered — so it
  // too passes its count. `TIFFFieldWithTag` returns null when the file did not
  // carry the tag at all, which is the common case and not an error.
  const TIFFField* field = TIFFFieldWithTag(tif, kTagGdalNoData);
  if (field == nullptr) {
    return std::nullopt;
  }
  std::string text;
  if (TIFFFieldPassCount(field) != 0) {
    std::uint32_t count = 0;
    void* data = nullptr;
    if (TIFFGetField(tif, kTagGdalNoData, &count, &data) != 1 || data == nullptr) {
      return std::nullopt;
    }
    text.assign(static_cast<const char*>(data), count);
  } else {
    char* raw = nullptr;
    if (TIFFGetField(tif, kTagGdalNoData, &raw) != 1 || raw == nullptr) {
      return std::nullopt;
    }
    text = raw;
  }
  if (const std::size_t nul = text.find('\0'); nul != std::string::npos) {
    text.resize(nul);
  }
  return proj_detail::parse_double(text);
}

} // namespace

Expected<GisRasterParseResult> load_geotiff(const std::filesystem::path& path) {
  const std::string source_name = path.filename().string();

  // Silence libtiff's default handlers, which print to stderr. The kernel has
  // no business writing to a stream (no iostream in core), and a warning the
  // user cannot see in the Diagnostics dock is a warning that did not happen.
  TIFFSetErrorHandler(nullptr);
  TIFFSetWarningHandler(nullptr);

  const TiffHandle handle(TIFFOpen(path.string().c_str(), "r"));
  if (handle.get() == nullptr) {
    return make_error(ErrorCode::InvalidDocument, "not a readable TIFF file", source_name);
  }
  TIFF* tif = handle.get();

  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) != 1 ||
      TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height) != 1 || width == 0 || height == 0) {
    return make_error(ErrorCode::InvalidDocument, "the TIFF declares no image size", source_name);
  }

  if (const Expected<void> capped = enforce_texel_budget(width, height, source_name);
      !capped.has_value()) {
    return make_error(capped.error().code, capped.error().message, capped.error().context);
  }

  std::uint16_t compression = COMPRESSION_NONE;
  TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
  if (TIFFIsCODECConfigured(compression) == 0) {
    return make_error(
        ErrorCode::InvalidDocument,
        fmt::format("the image is {}-compressed, which this build does not decode — supported: "
                    "uncompressed, PackBits and LZW; see issue #484",
                    compression_name(compression)),
        source_name);
  }

  GisRasterParseResult result;
  GisRaster& raster = result.raster;
  raster.width = static_cast<int>(width);
  raster.height = static_cast<int>(height);
  raster.crs = crs_from_geokeys(tif, result.diagnostics, source_name);

  const std::optional<std::array<double, 6>> transform = read_transform(tif);
  if (!transform.has_value()) {
    return make_error(
        ErrorCode::InvalidDocument,
        "the file carries no GeoTIFF positioning tags (ModelPixelScale + ModelTiepoint, or "
        "ModelTransformation), so it does not say where on the earth it is",
        source_name);
  }
  raster.transform = *transform;
  raster.nodata = read_nodata(tif);

  std::uint16_t samples = 1;
  std::uint16_t bits = 8;
  std::uint16_t format = SAMPLEFORMAT_UINT;
  std::uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);

  // A single band that is not 8-bit, or is explicitly floating point, is
  // elevation rather than a picture. An 8-bit single band is a greyscale image:
  // it could be a heavily quantised DEM, but reading it as one would invent 256
  // discrete heights out of what is almost always a picture.
  const bool single_band = samples == 1;
  const bool wide_or_float = bits > 8 || format == SAMPLEFORMAT_IEEEFP;
  raster.elevation = single_band && wide_or_float;

  if (raster.elevation) {
    // Elevation is read band-natively through the scanline API — RGBAImage
    // would quantise it to 8 bits, which is the whole point of not using it.
    if (TIFFIsTiled(tif) != 0) {
      return make_error(ErrorCode::InvalidDocument,
                        "tiled single-band elevation rasters are not read by this build; a "
                        "stripped GeoTIFF of the same data imports",
                        source_name);
    }
    const tmsize_t scanline = TIFFScanlineSize(tif);
    if (scanline <= 0) {
      return make_error(
          ErrorCode::InvalidDocument, "the TIFF declares an empty scanline", source_name);
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(scanline));
    raster.band.assign(raster.texel_count(), 0.0F);
    for (std::uint32_t y = 0; y < height; ++y) {
      if (TIFFReadScanline(tif, row.data(), y) < 0) {
        return make_error(ErrorCode::InvalidDocument,
                          fmt::format("the image data ends at row {} of {}", y, height),
                          source_name);
      }
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::size_t index = (static_cast<std::size_t>(y) * width) + x;
        const std::size_t byte = static_cast<std::size_t>(x) * (bits / 8U);
        if (byte + (bits / 8U) > row.size()) {
          break;
        }
        double value = 0.0;
        if (format == SAMPLEFORMAT_IEEEFP && bits == 32) {
          float f = 0.0F;
          std::memcpy(&f, row.data() + byte, sizeof(f));
          value = f;
        } else if (format == SAMPLEFORMAT_IEEEFP && bits == 64) {
          double d = 0.0;
          std::memcpy(&d, row.data() + byte, sizeof(d));
          value = d;
        } else if (format == SAMPLEFORMAT_INT && bits == 16) {
          std::int16_t v = 0;
          std::memcpy(&v, row.data() + byte, sizeof(v));
          value = v;
        } else if (format == SAMPLEFORMAT_INT && bits == 32) {
          std::int32_t v = 0;
          std::memcpy(&v, row.data() + byte, sizeof(v));
          value = v;
        } else if (bits == 16) {
          std::uint16_t v = 0;
          std::memcpy(&v, row.data() + byte, sizeof(v));
          value = v;
        } else if (bits == 32) {
          std::uint32_t v = 0;
          std::memcpy(&v, row.data() + byte, sizeof(v));
          value = v;
        } else {
          return make_error(ErrorCode::InvalidDocument,
                            fmt::format("a {}-bit sample format this build does not read", bits),
                            source_name);
        }
        raster.band[index] = static_cast<float>(value);
      }
    }
    result.diagnostics.push_back(
        Diagnostic{.severity = Severity::Info,
                   .location = source_name,
                   .message = fmt::format("read as a {}×{} elevation raster ({}-bit {})",
                                          width,
                                          height,
                                          bits,
                                          format == SAMPLEFORMAT_IEEEFP ? "float" : "integer")});
    return result;
  }

  // Imagery. TIFFReadRGBAImageOriented handles every strip/tile, planar and
  // photometric combination libtiff supports and hands back straight RGBA,
  // which is exactly the texture upload format the renderer wants.
  raster.rgba.assign(raster.texel_count() * 4, 0);
  std::vector<std::uint32_t> pixels(raster.texel_count());
  if (TIFFReadRGBAImageOriented(tif, width, height, pixels.data(), ORIENTATION_TOPLEFT, 0) == 0) {
    return make_error(
        ErrorCode::InvalidDocument, "the image data could not be decoded", source_name);
  }
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t p = pixels[i];
    raster.rgba[(i * 4) + 0] = static_cast<std::uint8_t>(TIFFGetR(p));
    raster.rgba[(i * 4) + 1] = static_cast<std::uint8_t>(TIFFGetG(p));
    raster.rgba[(i * 4) + 2] = static_cast<std::uint8_t>(TIFFGetB(p));
    raster.rgba[(i * 4) + 3] = static_cast<std::uint8_t>(TIFFGetA(p));
  }

  result.diagnostics.push_back(
      Diagnostic{.severity = Severity::Info,
                 .location = source_name,
                 .message = fmt::format("read as a {}×{} image ({}, {} sample{} per pixel)",
                                        width,
                                        height,
                                        compression_name(compression),
                                        samples,
                                        samples == 1 ? "" : "s")});
  return result;
}

} // namespace roadmaker::gis
