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

} // namespace
} // namespace roadmaker::editor
