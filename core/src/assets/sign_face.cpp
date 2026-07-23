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

// CPU text-to-texture rasteriser for editable sign faces. The single
// STB_TRUETYPE_IMPLEMENTATION translation unit in the kernel — stb_truetype.h is
// pulled in as a SYSTEM header (angle brackets, via the rm_stb include dir) so
// its implementation compiles clean under -Werror, exactly like the tinygltf
// implementation TU. STB_TRUETYPE_STATIC keeps every stbtt symbol internal so
// the shared-kernel export check sees only our RM_API surface.

#include "roadmaker/assets/sign_face.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "sign_font.hpp" // core-private embedded font bytes

#define STB_TRUETYPE_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

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

  const std::array<unsigned char, 3> bg = to_u8(plate.background);
  const std::array<unsigned char, 3> ink = to_u8(plate.ink);
  const auto texel = [&](int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x)) *
           4;
  };

  // Opaque background fill.
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t o = texel(x, y);
      bmp.rgba[o + 0] = bg[0];
      bmp.rgba[o + 1] = bg[1];
      bmp.rgba[o + 2] = bg[2];
      bmp.rgba[o + 3] = 255;
    }
  }

  const std::vector<std::vector<int>> lines = split_lines_utf8(text);
  const bool any_glyph =
      std::any_of(lines.begin(), lines.end(), [](const auto& l) { return !l.empty(); });
  if (!any_glyph) {
    return bmp; // plain plate
  }

  const std::span<const unsigned char> font_bytes = font_data();
  stbtt_fontinfo font;
  if (stbtt_InitFont(&font, font_bytes.data(), stbtt_GetFontOffsetForIndex(font_bytes.data(), 0)) ==
      0) {
    return bmp; // font failed to load (should not happen with the embedded font)
  }

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

  // A 4-texel ink-free margin on every side; text is fit inside the rest.
  constexpr int kMargin = 4;
  const int avail_w = std::max(1, width - 2 * kMargin);
  const int avail_h = std::max(1, height - 2 * kMargin);

  const float scale_w = static_cast<float>(avail_w) / static_cast<float>(max_line_width);
  const float scale_h = static_cast<float>(avail_h) / static_cast<float>(num_lines * line_advance);
  const float scale = std::min(scale_w, scale_h);

  const float block_h = static_cast<float>(num_lines * line_advance) * scale;
  const float block_top = (static_cast<float>(height) - block_h) * 0.5f;

  for (int li = 0; li < num_lines; ++li) {
    const auto& line = lines[static_cast<std::size_t>(li)];
    const float line_w_px = static_cast<float>(line_advance_width(font, line)) * scale;
    float pen_x = (static_cast<float>(width) - line_w_px) * 0.5f;
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
          // Clip to the ink-free margin box, guaranteeing a clean border.
          if (dy < kMargin || dy >= height - kMargin) {
            continue;
          }
          for (int gx = 0; gx < gw; ++gx) {
            const int dx = dst_x0 + gx;
            if (dx < kMargin || dx >= width - kMargin) {
              continue;
            }
            const float a =
                static_cast<float>(
                    coverage[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gw) +
                             static_cast<std::size_t>(gx)]) /
                255.0f;
            if (a <= 0.0f) {
              continue;
            }
            const std::size_t o = texel(dx, dy);
            for (int ch = 0; ch < 3; ++ch) {
              const float mixed = static_cast<float>(ink[static_cast<std::size_t>(ch)]) * a +
                                  static_cast<float>(bg[static_cast<std::size_t>(ch)]) * (1.0f - a);
              bmp.rgba[o + static_cast<std::size_t>(ch)] =
                  static_cast<unsigned char>(std::lround(mixed));
            }
          }
        }
        stbtt_FreeBitmap(coverage, nullptr);
      }
      pen_x += static_cast<float>(advance) * scale;
    }
  }
  return bmp;
}

} // namespace roadmaker::signs
