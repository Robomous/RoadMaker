#pragma once

#include <span>

/// Internal (core-private) access to the embedded sign-face font. The bytes are
/// a Latin subset of Roboto-Regular (Apache-2.0), compiled in from the generated
/// sign_font.gen.cpp so the rasteriser needs no runtime file IO. Not part of the
/// public kernel API — only src/assets/sign_face.cpp consumes it.
namespace roadmaker::signs {

/// The embedded TrueType font bytes, valid for the program lifetime.
std::span<const unsigned char> font_data();

} // namespace roadmaker::signs
