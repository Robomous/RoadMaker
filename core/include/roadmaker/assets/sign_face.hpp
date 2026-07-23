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

#include "roadmaker/assets/prop_library.hpp" // props::FacePlate
#include "roadmaker/export.hpp"

#include <string_view>
#include <vector>

/// CPU text-to-texture for editable sign faces. One deterministic rasteriser,
/// consumed by BOTH the editor viewport (uploaded as a GL texture) and the glTF
/// exporter (embedded as a PNG in the .glb), so a sign's text looks identical
/// wherever it is drawn or exported. Lives in core (never Qt) because the glTF
/// exporter runs headless and from Python.
namespace roadmaker::signs {

/// An RGBA8 raster of a sign face. Row-major, **row 0 = top**, 4 bytes/px, fully
/// opaque (alpha 255 — no blending or draw-order concerns downstream). width and
/// height are texels; rgba.size() == width * height * 4.
struct FaceBitmap {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> rgba;
};

/// Rasterise `text` onto `plate`. Contract:
/// - RGBA8, row 0 = top, opaque alpha 255. Colours are the plate's linear-RGB
///   background and ink written straight to 8-bit (same gamma treatment as the
///   flat prop materials).
/// - `'\n'` splits lines; the line block is centred both ways; each line is
///   centred horizontally. A carriage return is ignored.
/// - The glyph pixel size fits the longest line to the plate width AND the whole
///   stack to the plate height (whichever is tighter), so text never overflows.
/// - A 4-texel ink-free margin is kept on every side (guards mipmap bleed): no
///   ink is ever written into that border.
/// - Empty (or whitespace-only) text ⇒ a plain plate: just the background fill.
/// - Input is UTF-8; unmapped codepoints draw the font's .notdef box and invalid
///   byte sequences render U+FFFD — never a crash.
/// The bitmap resolution derives from the plate aspect ratio (longer side ≈ 256
/// texels, each dimension rounded to a multiple of 4).
[[nodiscard]] RM_API FaceBitmap render_face(std::string_view text, const props::FacePlate& plate);

} // namespace roadmaker::signs
