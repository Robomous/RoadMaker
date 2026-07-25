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

// Tests for the CPU sign-face rasteriser (roadmaker::signs::render_face). stb's
// float rasteriser can differ by ±1 LSB across platforms, so these assert
// STRUCTURE — ink present/absent per region, a clean margin, an opaque plate —
// never golden pixels.

#include "roadmaker/assets/sign_face.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace roadmaker::signs {
namespace {

// A generic yellow-on-black test plate (the StVO 310 look), independent of the
// bundled model so the rasteriser is exercised in isolation.
props::FacePlate make_plate() {
  props::FacePlate plate;
  plate.x = 0.045;
  plate.z = 2.55;
  plate.half_w = 0.55;
  plate.half_h = 0.33;
  plate.background = {0.93f, 0.75f, 0.10f};
  plate.ink = {0.09f, 0.09f, 0.10f};
  return plate;
}

// True if any texel in the half-open row band [y0, y1) carries ink. The yellow
// background has red ≈238 and the near-black ink has red ≈23, so a substantially
// darkened red channel is a clean, gamma-tolerant ink discriminator.
bool has_ink(const FaceBitmap& bmp, int y0, int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = 0; x < bmp.width; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width) +
                             static_cast<std::size_t>(x)) *
                            4;
      if (bmp.rgba[o + 0] < 150) {
        return true;
      }
    }
  }
  return false;
}

TEST(SignFace, NonEmptyTextInksThePlate) {
  const FaceBitmap bmp = render_face("City", make_plate());
  EXPECT_GT(bmp.width, 0);
  EXPECT_GT(bmp.height, 0);
  EXPECT_EQ(bmp.rgba.size(),
            static_cast<std::size_t>(bmp.width) * static_cast<std::size_t>(bmp.height) * 4u);
  EXPECT_TRUE(has_ink(bmp, 0, bmp.height)) << "text should darken some texels";
}

TEST(SignFace, MultiLineStacksLines) {
  // The spec's own @text example. Ink must appear in BOTH vertical halves.
  const FaceBitmap bmp = render_face("City\nBadAibling", make_plate());
  const int mid = bmp.height / 2;
  EXPECT_TRUE(has_ink(bmp, 0, mid)) << "first line missing from top half";
  EXPECT_TRUE(has_ink(bmp, mid, bmp.height)) << "second line missing from bottom half";
}

TEST(SignFace, LongLineStaysInsideMargin) {
  const FaceBitmap bmp = render_face("A very long town name that must fit", make_plate());
  constexpr int kMargin = 4;
  // The 4-texel border ring must be pure background — no ink bleeds out.
  EXPECT_FALSE(has_ink(bmp, 0, kMargin)) << "top margin inked";
  EXPECT_FALSE(has_ink(bmp, bmp.height - kMargin, bmp.height)) << "bottom margin inked";
  // Left/right margin columns.
  for (int y = 0; y < bmp.height; ++y) {
    for (int x = 0; x < kMargin; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width) +
                             static_cast<std::size_t>(x)) *
                            4;
      EXPECT_GE(bmp.rgba[o + 0], 150) << "left margin inked at row " << y;
    }
    for (int x = bmp.width - kMargin; x < bmp.width; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width) +
                             static_cast<std::size_t>(x)) *
                            4;
      EXPECT_GE(bmp.rgba[o + 0], 150) << "right margin inked at row " << y;
    }
  }
}

TEST(SignFace, EmptyTextIsPlainPlate) {
  const FaceBitmap bmp = render_face("", make_plate());
  ASSERT_FALSE(bmp.rgba.empty());
  for (std::size_t i = 0; i < bmp.rgba.size(); i += 4) {
    EXPECT_EQ(bmp.rgba[i + 3], 255) << "plate must be opaque";
  }
  EXPECT_FALSE(has_ink(bmp, 0, bmp.height)) << "empty text must leave a plain plate";
}

TEST(SignFace, WhitespaceOnlyTextIsPlainPlate) {
  const FaceBitmap bmp = render_face("   ", make_plate());
  // Spaces carry no ink coverage, so the plate stays plain.
  EXPECT_FALSE(has_ink(bmp, 0, bmp.height));
}

TEST(SignFace, InvalidUtf8DoesNotCrash) {
  // Lone continuation bytes, a truncated multi-byte lead, and a stray 0xFF.
  const std::string bad = std::string("A\xFF\x80\xE2\x28") + "Z";
  const FaceBitmap bmp = render_face(bad, make_plate());
  EXPECT_GT(bmp.width, 0);
  EXPECT_EQ(bmp.rgba.size(),
            static_cast<std::size_t>(bmp.width) * static_cast<std::size_t>(bmp.height) * 4u);
}

TEST(SignFace, OpaqueEverywhere) {
  const FaceBitmap bmp = render_face("City\nBadAibling", make_plate());
  for (std::size_t i = 3; i < bmp.rgba.size(); i += 4) {
    EXPECT_EQ(bmp.rgba[i], 255);
  }
}

// --- artwork, fixed legends and boxes (US sign pack, #414) -------------------

/// Count texels that differ from the plate's flat fill — "did anything draw
/// here", never a golden-pixel comparison.
std::size_t ink_texels(const FaceBitmap& bmp, const props::FacePlate& plate) {
  const auto to_u8 = [](float v) {
    return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  const unsigned char r = to_u8(plate.background[0]);
  const unsigned char g = to_u8(plate.background[1]);
  const unsigned char b = to_u8(plate.background[2]);
  std::size_t count = 0;
  for (std::size_t o = 0; o + 3 < bmp.rgba.size(); o += 4) {
    if (bmp.rgba[o] != r || bmp.rgba[o + 1] != g || bmp.rgba[o + 2] != b) {
      ++count;
    }
  }
  return count;
}

TEST(SignFace, BundledArtworkDrawsWithNoText) {
  // The whole point of the symbol layer: a Do Not Enter face carries no @text
  // at all and must still render its roundel.
  props::FacePlate plate = make_plate();
  plate.symbol = "R5-1";
  const FaceBitmap bmp = render_face("", plate);
  EXPECT_GT(ink_texels(bmp, plate), 0U) << "artwork must composite over the fill";
}

TEST(SignFace, AnUnknownArtworkKeyIsAPlainPlate) {
  // A catalogue can outrun the asset bundle. That must degrade to a blank
  // plate, never to a crash or a hole in the texture.
  props::FacePlate plate = make_plate();
  plate.symbol = "no-such-designation";
  const FaceBitmap bmp = render_face("", plate);
  EXPECT_EQ(ink_texels(bmp, plate), 0U);
  for (std::size_t i = 3; i < bmp.rgba.size(); i += 4) {
    EXPECT_EQ(bmp.rgba[i], 255) << "a degraded face is still opaque";
  }
}

TEST(SignFace, LegendAndTextDrawInTheirOwnBoxes) {
  // A speed-limit face: the SPEED LIMIT wordmark up top, the posted number
  // below. Each layer must stay inside its box, or the two overprint.
  props::FacePlate plate = make_plate();
  plate.legend = "SPEED\nLIMIT";
  plate.legend_ink = {0.0f, 0.0f, 0.0f};
  plate.legend_box = {0.05, 0.04, 0.90, 0.34};
  plate.text_box = {0.10, 0.42, 0.80, 0.54};

  const FaceBitmap legend_only = render_face("", plate);
  const FaceBitmap both = render_face("25", plate);
  EXPECT_GT(ink_texels(legend_only, plate), 0U) << "the fixed legend draws on its own";
  EXPECT_GT(ink_texels(both, plate), ink_texels(legend_only, plate))
      << "the editable value adds ink below the wordmark";

  // The wordmark's band must be untouched by the value, and vice versa.
  const int split = static_cast<int>(std::lround(0.40 * both.height));
  const auto band_ink = [&](const FaceBitmap& bmp, int y0, int y1) {
    std::size_t count = 0;
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < bmp.width; ++x) {
        const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.width) +
                               static_cast<std::size_t>(x)) *
                              4;
        if (bmp.rgba[o] != legend_only.rgba[o]) {
          ++count;
        }
      }
    }
    return count;
  };
  EXPECT_EQ(band_ink(both, 0, split), 0U) << "the value must not reach the wordmark's band";
  EXPECT_GT(band_ink(both, split, both.height), 0U) << "the value draws in its own band";
}

TEST(SignFace, ArtworkAndLegendCompose) {
  // A ONE WAY face is both at once: the arrow is artwork, the words are a
  // legend. Together they must draw more than either alone.
  props::FacePlate plate = make_plate();
  plate.symbol = "R6-1-right";
  plate.legend = "ONE WAY";
  plate.legend_ink = {1.0f, 1.0f, 1.0f};
  plate.legend_box = {0.03, 0.18, 0.38, 0.64};

  props::FacePlate symbol_only = plate;
  symbol_only.legend.clear();
  props::FacePlate legend_only = plate;
  legend_only.symbol.clear();

  const std::size_t both = ink_texels(render_face("", plate), plate);
  EXPECT_GT(both, ink_texels(render_face("", symbol_only), plate));
  EXPECT_GT(both, ink_texels(render_face("", legend_only), plate));
}

} // namespace
} // namespace roadmaker::signs
