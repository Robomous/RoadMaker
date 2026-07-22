// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Theme registry and application smoke — token VALUES are a maintainer
// pick, not a testable invariant (ui-design.md); what must hold is the
// lookup contract and that apply() actually themes the application.

#include <gtest/gtest.h>

#include <QApplication>
#include <QPalette>

#include "theme/theme.hpp"

namespace roadmaker::editor {
namespace {

TEST(Theme, RegistryRoundTripsEveryAvailableName) {
  const QStringList names = theme::available();
  EXPECT_EQ(names.size(), 3);
  for (const QString& name : names) {
    const Theme* found = theme::by_name(name);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, name);
  }
  EXPECT_EQ(theme::by_name(QStringLiteral("not-a-theme")), nullptr);
}

TEST(Theme, BackdropConvertsTokensIntoUnitRangeFloats) {
  const BackdropColors colors = theme::slate_cyan().backdrop();
  for (const float c : colors.sky_top) {
    EXPECT_GE(c, 0.0F);
    EXPECT_LE(c, 1.0F);
  }
  EXPECT_GT(colors.grid_major[3], 0.0F); // grid lines must not vanish
  EXPECT_GT(colors.grid_minor[3], 0.0F);
}

TEST(Theme, ApplySetsPaletteStyleSheetAndCurrent) {
  auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
  ASSERT_NE(app, nullptr);
  theme::apply(*app, theme::warm_signal());
  EXPECT_EQ(theme::current().name, theme::warm_signal().name);
  EXPECT_FALSE(app->styleSheet().isEmpty());
  EXPECT_EQ(app->palette().color(QPalette::Highlight), theme::warm_signal().accent);
  // No token placeholder may survive substitution into the shipped QSS.
  EXPECT_FALSE(app->styleSheet().contains(QLatin1Char('@')));
  theme::apply(*app, theme::default_theme()); // leave the suite on defaults
}

} // namespace
} // namespace roadmaker::editor
