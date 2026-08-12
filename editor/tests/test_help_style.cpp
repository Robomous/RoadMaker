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

// The in-app help stylesheet is generated from the theme tokens; the committed
// editor/resources/help/help.css (baked into the shipped .qch) must be exactly
// what help_style::css(default theme) produces. This gate is the tie.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "theme/help_style.hpp"
#include "theme/theme.hpp"

namespace roadmaker::editor {
namespace {

TEST(HelpStyle, CssMatchesCommittedHelpCss) {
  const std::filesystem::path path =
      std::filesystem::path(RM_EDITOR_RESOURCES_DIR) / "help" / "help.css";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.is_open()) << "missing " << path.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  const QString committed = QString::fromStdString(buffer.str());

  const QString generated = help_style::css(theme::default_theme());
  EXPECT_EQ(committed, generated)
      << "editor/resources/help/help.css is out of date with help_style::css().\n"
         "Replace the committed file with:\n\n"
      << generated.toStdString();
}

TEST(HelpStyle, CssSubstitutesEveryToken) {
  // No '@token' placeholder may survive substitution.
  EXPECT_FALSE(help_style::css(theme::default_theme()).contains(QLatin1Char('@')));
  EXPECT_FALSE(help_style::css(theme::slate_cyan()).contains(QLatin1Char('@')));
  EXPECT_FALSE(help_style::css(theme::warm_signal()).contains(QLatin1Char('@')));
}

TEST(HelpStyle, CssUsesDefaultThemeAccent) {
  const Theme& t = theme::default_theme();
  EXPECT_TRUE(help_style::css(t).contains(t.accent.name()))
      << "the default accent " << t.accent.name().toStdString() << " must drive link colour";
}

// The same tie for the documentation site's palette (#563). It replaced
// docs-site/scripts/theme-css.mjs, which parsed theme.cpp with `indexOf` and a
// `QColor(0x..)` regex — this gate is what lets the site read a committed file
// instead, without putting a C++ toolchain on the Node-only site build.
//
// docs-site/src/styles/theme.css is pinned `text eol=lf` in .gitattributes: the
// generator emits '\n' and this reads in binary, so without the pin the Windows
// runner's autocrlf checkout fails on line endings alone — invisible on macOS
// and Linux, and this repository has been bitten by it three times already.
TEST(HelpStyle, StarlightCssMatchesCommittedThemeCss) {
  const std::filesystem::path path =
      std::filesystem::path(RM_REPO_ROOT_DIR) / "docs-site" / "src" / "styles" / "theme.css";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file.is_open()) << "missing " << path.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  const QString committed = QString::fromStdString(buffer.str());

  // graphite_amber, not default_theme(): the site ships one palette, and it is
  // named explicitly so renaming the default cannot silently repaint the site.
  const QString generated = help_style::starlight_css(theme::graphite_amber());
  EXPECT_EQ(committed, generated)
      << "docs-site/src/styles/theme.css is out of date with "
         "help_style::starlight_css().\nReplace the committed file with:\n\n"
      << generated.toStdString();
}

TEST(HelpStyle, StarlightCssSubstitutesEveryPlaceholder) {
  // A leftover %1..%9 means an .arg() was dropped and the site would ship the
  // placeholder as a colour value.
  const QString css = help_style::starlight_css(theme::graphite_amber());
  for (int i = 1; i <= 9; ++i) {
    EXPECT_FALSE(css.contains(QStringLiteral("%%%1").arg(i)))
        << "placeholder %" << i << " survived substitution";
  }
}

} // namespace
} // namespace roadmaker::editor
