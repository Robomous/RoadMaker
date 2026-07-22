// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "theme/help_style.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace roadmaker::editor::help_style {

QString css(const Theme& t) {
  // One template, tokens injected by name (mirrors theme.cpp build_qss). Longest
  // token names are replaced first so shared prefixes (@border vs
  // @borderStrong) cannot mis-substitute. QTextBrowser rich-text subset only.
  QString sheet =
      QStringLiteral(R"css(/* RoadMaker in-app help stylesheet — GENERATED from theme tokens by
   help_style::css() (editor/src/theme/help_style.cpp). Do not edit by hand:
   the CssMatchesCommittedHelpCss gate regenerates it. QTextBrowser rich-text
   subset only — no flex, no grid, no media queries. */

body { background-color: @bg; color: @text;
       font-family: -apple-system, "Segoe UI", "Noto Sans", sans-serif;
       font-size: 15px; }

h1 { color: @text; font-size: 26px; font-weight: 700;
     border-bottom: 1px solid @border; padding-bottom: 6px; }
h2 { color: @text; font-size: 20px; font-weight: 600;
     border-bottom: 1px solid @border; padding-bottom: 4px; }
h3 { color: @text; font-size: 16px; font-weight: 600; }
h4, h5, h6 { color: @textSec; font-weight: 600; }

p, li { color: @text; line-height: 150%; }
em { color: @textSec; }
strong { color: @text; font-weight: 700; }

a { color: @accent; text-decoration: none; }

code { background-color: @bgCode; color: @text;
       font-family: "SF Mono", "Consolas", monospace; font-size: 13px; }
pre { background-color: @bgCode; color: @text; border: 1px solid @border;
      padding: 8px; font-family: "SF Mono", "Consolas", monospace;
      font-size: 13px; }

blockquote { color: @textSec; border-left: 3px solid @borderStrong;
             padding-left: 10px; }

table { border: 1px solid @border; }
th { background-color: @bgCode; color: @text; border: 1px solid @border;
     padding: 4px 8px; text-align: left; }
td { color: @text; border: 1px solid @border; padding: 4px 8px; }
)css");

  std::vector<std::pair<QString, QString>> tokens = {
      {QStringLiteral("@borderStrong"), t.border_strong.name()},
      {QStringLiteral("@border"), t.border.name()},
      {QStringLiteral("@bgCode"), t.bg2.name()},
      {QStringLiteral("@bg"), t.bg1.name()},
      {QStringLiteral("@textSec"), t.text_secondary.name()},
      {QStringLiteral("@text"), t.text_primary.name()},
      {QStringLiteral("@accent"), t.accent.name()},
  };
  std::ranges::sort(tokens,
                    [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  for (const auto& [token, value] : tokens) {
    sheet.replace(token, value);
  }
  return sheet;
}

} // namespace roadmaker::editor::help_style
