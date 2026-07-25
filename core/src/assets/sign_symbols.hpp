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

#include <span>
#include <string_view>

/// Internal (core-private) access to the embedded US sign-pack artwork. The
/// bytes are the SVG *source text* of assets/signs/us/*.svg, compiled in from
/// the generated sign_symbols.gen.cpp so the face rasteriser needs no runtime
/// file IO and can render each face at whatever size it asks for. Not part of
/// the public kernel API — only src/assets/sign_face.cpp consumes it.
namespace roadmaker::signs {

/// The embedded SVG bytes for `key` (a designation, e.g. "R1-1"), or an empty
/// span when this build ships no artwork under that key. Valid for the program
/// lifetime.
std::span<const unsigned char> symbol_data(std::string_view key);

} // namespace roadmaker::signs
