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

// CPU rasteriser for sign faces: flat fill, then the sign's SVG artwork, then
// its fixed legend and its editable @text. The single STB_TRUETYPE_IMPLEMENTATION
// and NANOSVG*_IMPLEMENTATION translation unit in the kernel — both headers are
// pulled in as SYSTEM headers (angle brackets, via the rm_stb / rm_nanosvg
// include dirs) so their implementations compile clean under -Werror, exactly
// like the tinygltf implementation TU. STB_TRUETYPE_STATIC keeps every stbtt
// symbol internal so the shared-kernel export check sees only our RM_API
// surface.
//
// nanosvg rather than Qt's renderer: core must never link Qt, and faces are
// baked headless by the glTF exporter and from Python. nanosvg parses shapes
// only — no <text> — which is why every legend is a text layer here rather than
// artwork, and why they all share the bundled highway typeface.

#include "roadmaker/assets/sign_face.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sign_font.hpp"    // core-private embedded font bytes
#include "sign_symbols.hpp" // core-private embedded SVG artwork

#define STB_TRUETYPE_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace roadmaker::signs {

namespace {

/// A linear-RGB colour in [0,1] written straight to an opaque 8-bit texel.
std::array<unsigned char, 3> to_u8(const std::array<float, 3>& c) {
  std::array<unsigned char, 3> out{};
  for (int i = 0; i < 3; ++i) {
    const float v = std::clamp(c[static_cast<std::size_t>(i)], 0.0f, 1.0f);
    out[static_cast<std::size_t>(i)] = static_cast<unsigned char>(std::lround(v * 255.0f));
  }
  return out;
}

/// Round down to a multiple of 4, never below 4 (POT-friendly, /4 alignment).
int round_down_4(int v) {
  const int r = (v / 4) * 4;
  return r < 4 ? 4 : r;
}

/// Decode UTF-8 `text` into lines of Unicode codepoints, splitting on '\n' and
/// dropping '\r'. Invalid byte sequences yield U+FFFD and never stall.
std::vector<std::vector<int>> split_lines_utf8(std::string_view text) {
  std::vector<std::vector<int>> lines(1);
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    int cp = 0;
    int len = 0;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x07;
      len = 4;
    } else {
      lines.back().push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + static_cast<std::size_t>(len) > text.size()) {
      lines.back().push_back(0xFFFD);
      ++i;
      continue;
    }
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
      if ((cc >> 6) != 0x2) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      lines.back().push_back(0xFFFD);
      ++i;
      continue;
    }
    i += static_cast<std::size_t>(len);
    if (cp == 0x0A) { // '\n'
      lines.emplace_back();
    } else if (cp != 0x0D) { // ignore '\r'
      lines.back().push_back(cp);
    }
  }
  return lines;
}

/// Sum of advance widths (font units) for one line — its unscaled pen width.
int line_advance_width(const stbtt_fontinfo& font, const std::vector<int>& line) {
  int width = 0;
  for (const int cp : line) {
    int advance = 0;
    int lsb = 0;
    stbtt_GetCodepointHMetrics(&font, cp, &advance, &lsb);
    width += advance;
  }
  return width;
}

/// A texel rectangle inside the bitmap, derived from a normalised FaceBox and
/// shrunk by the ink-free margin. Never empty: a degenerate box collapses to a
/// single texel rather than making the caller check.
struct PixelBox {
  int x0 = 0;
  int y0 = 0;
  int x1 = 1; ///< exclusive
  int y1 = 1; ///< exclusive

  [[nodiscard]] int width() const { return x1 - x0; }

  [[nodiscard]] int height() const { return y1 - y0; }
};

/// A 4-texel ink-free margin on every side of the FACE (not of each box), so
/// the border stays clean for mipmapping however the boxes are laid out.
constexpr int kMargin = 4;

PixelBox to_pixels(const props::FaceBox& box, int width, int height) {
  const auto span = [](double origin, double extent, int total) {
    const int lo = static_cast<int>(std::lround(std::clamp(origin, 0.0, 1.0) * total));
    const int hi =
        static_cast<int>(std::lround(std::clamp(origin + std::max(extent, 0.0), 0.0, 1.0) * total));
    return std::pair<int, int>{lo, std::max(hi, lo + 1)};
  };
  const auto [x0, x1] = span(box[0], box[2], width);
  const auto [y0, y1] = span(box[1], box[3], height);
  PixelBox out;
  out.x0 = std::max(x0, kMargin);
  out.y0 = std::max(y0, kMargin);
  out.x1 = std::min(x1, width - kMargin);
  out.y1 = std::min(y1, height - kMargin);
  out.x1 = std::max(out.x1, out.x0 + 1);
  out.y1 = std::max(out.y1, out.y0 + 1);
  return out;
}

/// Composite the bundled artwork for `key` over `bmp`, fitted to the face and
/// centred. A key this build ships no artwork for, or one nanosvg refuses, is a
/// silent no-op: a sign whose artwork is missing must still render as a plate,
/// never as a crash or a hole.
void draw_symbol(FaceBitmap& bmp, std::string_view key) {
  const std::span<const unsigned char> svg = symbol_data(key);
  if (svg.empty()) {
    return;
  }
  // nsvgParse mutates its input, so hand it a private null-terminated copy.
  std::string source(reinterpret_cast<const char*>(svg.data()), svg.size());
  const std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(
      nsvgParse(source.data(), "px", 96.0F), &nsvgDelete);
  if (image == nullptr || !(image->width > 0.0F) || !(image->height > 0.0F)) {
    return;
  }
  const std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(
      nsvgCreateRasterizer(), &nsvgDeleteRasterizer);
  if (rasterizer == nullptr) {
    return;
  }

  // Uniform fit, centred — the artwork's viewBox already matches the face
  // aspect, so this only absorbs the multiple-of-4 rounding.
  const float scale = std::min(static_cast<float>(bmp.width) / image->width,
                               static_cast<float>(bmp.height) / image->height);
  const float tx = (static_cast<float>(bmp.width) - image->width * scale) * 0.5F;
  const float ty = (static_cast<float>(bmp.height) - image->height * scale) * 0.5F;

  std::vector<unsigned char> layer(
      static_cast<std::size_t>(bmp.width) * static_cast<std::size_t>(bmp.height) * 4, 0);
  nsvgRasterize(rasterizer.get(),
                image.get(),
                tx,
                ty,
                scale,
                layer.data(),
                bmp.width,
                bmp.height,
                bmp.width * 4);

  // Source-over onto the opaque background; the face stays fully opaque.
  for (std::size_t o = 0; o + 3 < layer.size(); o += 4) {
    const float a = static_cast<float>(layer[o + 3]) / 255.0F;
    if (a <= 0.0F) {
      continue;
    }
    for (std::size_t ch = 0; ch < 3; ++ch) {
      const float mixed =
          static_cast<float>(layer[o + ch]) * a + static_cast<float>(bmp.rgba[o + ch]) * (1.0F - a);
      bmp.rgba[o + ch] = static_cast<unsigned char>(std::lround(mixed));
    }
  }
}

/// Draw `text` into `box`, centred both ways, scaled to fit. Multi-line splits
/// on '\n'. Glyphs blend onto whatever is already in the bitmap (the fill and
/// the artwork), and are clipped to the box so two layers never bleed into each
/// other. A no-glyph string is a no-op.
void draw_text(FaceBitmap& bmp,
               const stbtt_fontinfo& font,
               std::string_view text,
               const props::FaceBox& box,
               const std::array<unsigned char, 3>& ink) {
  const std::vector<std::vector<int>> lines = split_lines_utf8(text);
  const bool any_glyph =
      std::any_of(lines.begin(), lines.end(), [](const auto& l) { return !l.empty(); });
  if (!any_glyph) {
    return;
  }
  const PixelBox area = to_pixels(box, bmp.width, bmp.height);

  int ascent = 0;
  int descent = 0;
  int line_gap = 0;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
  const int line_advance = std::max(ascent - descent + line_gap, 1);

  int max_line_width = 1;
  for (const auto& line : lines) {
    max_line_width = std::max(max_line_width, line_advance_width(font, line));
  }
  const int num_lines = static_cast<int>(lines.size());

  const float scale_w = static_cast<float>(area.width()) / static_cast<float>(max_line_width);
  const float scale_h =
      static_cast<float>(area.height()) / static_cast<float>(num_lines * line_advance);
  const float scale = std::min(scale_w, scale_h);

  const float block_h = static_cast<float>(num_lines * line_advance) * scale;
  const float block_top =
      static_cast<float>(area.y0) + (static_cast<float>(area.height()) - block_h) * 0.5F;

  const auto texel = [&bmp](int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width) +
            static_cast<std::size_t>(x)) *
           4;
  };

  for (int li = 0; li < num_lines; ++li) {
    const auto& line = lines[static_cast<std::size_t>(li)];
    const float line_w_px = static_cast<float>(line_advance_width(font, line)) * scale;
    float pen_x =
        static_cast<float>(area.x0) + (static_cast<float>(area.width()) - line_w_px) * 0.5F;
    const float baseline_y = block_top + static_cast<float>(li * line_advance) * scale +
                             static_cast<float>(ascent) * scale;

    for (const int cp : line) {
      int advance = 0;
      int lsb = 0;
      stbtt_GetCodepointHMetrics(&font, cp, &advance, &lsb);

      int gw = 0;
      int gh = 0;
      int gx_off = 0;
      int gy_off = 0;
      unsigned char* coverage =
          stbtt_GetCodepointBitmap(&font, scale, scale, cp, &gw, &gh, &gx_off, &gy_off);
      if (coverage != nullptr) {
        const int dst_x0 = static_cast<int>(std::lround(pen_x)) + gx_off;
        const int dst_y0 = static_cast<int>(std::lround(baseline_y)) + gy_off;
        for (int gy = 0; gy < gh; ++gy) {
          const int dy = dst_y0 + gy;
          if (dy < area.y0 || dy >= area.y1) {
            continue;
          }
          for (int gx = 0; gx < gw; ++gx) {
            const int dx = dst_x0 + gx;
            if (dx < area.x0 || dx >= area.x1) {
              continue;
            }
            const float a =
                static_cast<float>(
                    coverage[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gw) +
                             static_cast<std::size_t>(gx)]) /
                255.0F;
            if (a <= 0.0F) {
              continue;
            }
            const std::size_t o = texel(dx, dy);
            for (std::size_t ch = 0; ch < 3; ++ch) {
              const float mixed = static_cast<float>(ink[ch]) * a +
                                  static_cast<float>(bmp.rgba[o + ch]) * (1.0F - a);
              bmp.rgba[o + ch] = static_cast<unsigned char>(std::lround(mixed));
            }
          }
        }
        stbtt_FreeBitmap(coverage, nullptr);
      }
      pen_x += static_cast<float>(advance) * scale;
    }
  }
}

} // namespace

FaceBitmap render_face(std::string_view text, const props::FacePlate& plate) {
  // Resolution from the plate aspect ratio: longer side ≈ 256 texels, each
  // dimension rounded to a multiple of 4.
  constexpr int kMaxSide = 256;
  const double w_m = std::max(plate.half_w * 2.0, 1e-6);
  const double h_m = std::max(plate.half_h * 2.0, 1e-6);
  int width = 0;
  int height = 0;
  if (w_m >= h_m) {
    width = kMaxSide;
    height = static_cast<int>(std::lround(kMaxSide * h_m / w_m));
  } else {
    height = kMaxSide;
    width = static_cast<int>(std::lround(kMaxSide * w_m / h_m));
  }
  width = round_down_4(width);
  height = round_down_4(height);

  FaceBitmap bmp;
  bmp.width = width;
  bmp.height = height;
  bmp.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);

  // Layer 1: opaque background fill.
  const std::array<unsigned char, 3> bg = to_u8(plate.background);
  for (std::size_t o = 0; o + 3 < bmp.rgba.size(); o += 4) {
    bmp.rgba[o + 0] = bg[0];
    bmp.rgba[o + 1] = bg[1];
    bmp.rgba[o + 2] = bg[2];
    bmp.rgba[o + 3] = 255;
  }

  // Layer 2: the sign's artwork.
  if (!plate.symbol.empty()) {
    draw_symbol(bmp, plate.symbol);
  }

  // Layers 3 and 4: the fixed legend, then the editable text. Both need the
  // font; if it fails to load (it cannot, it is compiled in) the face is still
  // a valid plate.
  if (plate.legend.empty() && text.empty()) {
    return bmp;
  }
  const std::span<const unsigned char> font_bytes = font_data();
  stbtt_fontinfo font;
  if (stbtt_InitFont(&font, font_bytes.data(), stbtt_GetFontOffsetForIndex(font_bytes.data(), 0)) ==
      0) {
    return bmp;
  }
  if (!plate.legend.empty()) {
    draw_text(bmp, font, plate.legend, plate.legend_box, to_u8(plate.legend_ink));
  }
  if (!text.empty()) {
    draw_text(bmp, font, text, plate.text_box, to_u8(plate.ink));
  }
  return bmp;
}

} // namespace roadmaker::signs
