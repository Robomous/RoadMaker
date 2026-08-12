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

// Stylesheet for the in-app help viewer (QTextBrowser). Generated from the same
// theme tokens as the editor chrome (theme.cpp build_qss), but constrained to
// the Qt rich-text CSS subset QTextBrowser understands — no flex, no grid, no
// media queries. The default-theme output is committed at
// editor/resources/help/help.css and baked into the shipped .qch at build time;
// a gtest keeps the committed file in step with this generator.

#include <QString>

#include "theme/theme.hpp"

namespace roadmaker::editor::help_style {

/// The full help.css contents for `theme`, including the generated-output
/// header comment (so the committed file is byte-for-byte this string).
[[nodiscard]] QString css(const Theme& theme);

/// The documentation site's Starlight custom-property block for `theme`, same
/// contract as `css()` above: the committed docs-site/src/styles/theme.css is
/// byte-for-byte this string, and a gtest keeps them in step (#563).
///
/// It lives here rather than in the site build because the site must not need a
/// C++ toolchain (ADR-0009 puts site CI on Linux-only Node runners). The
/// previous arrangement had docs-site/scripts/theme-css.mjs LOCATE
/// `graphite_amber()` in theme.cpp with `indexOf` and regex the `QColor(0x..)`
/// literals back out — a JavaScript parser of C++ source that any reformat
/// breaks. Generating here and byte-gating the committed file is the same shape
/// help.css has always used, and it keeps the two builds unrelated.
[[nodiscard]] QString starlight_css(const Theme& theme);

} // namespace roadmaker::editor::help_style
