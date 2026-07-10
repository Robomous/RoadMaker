#include <gtest/gtest.h>

#include <QIcon>
#include <QPixmap>

#include "app/icons.hpp"

namespace {

using roadmaker::editor::Icons;

TEST(Icons, BundledIconLoadsAndRenders) {
  const QIcon icon = Icons::get(QStringLiteral("clothoid-road"));
  ASSERT_FALSE(icon.isNull());

  const QPixmap pixmap = icon.pixmap(16);
  EXPECT_FALSE(pixmap.isNull());
  EXPECT_GT(pixmap.width(), 0);
}

TEST(Icons, DisabledStateIsProvided) {
  const QIcon icon = Icons::get(QStringLiteral("clothoid-road"));
  const QPixmap disabled = icon.pixmap(16, QIcon::Disabled);
  EXPECT_FALSE(disabled.isNull());
}

TEST(Icons, RepeatedLookupsAreCachedToTheSameIcon) {
  const QIcon first = Icons::get(QStringLiteral("clothoid-road"));
  const QIcon second = Icons::get(QStringLiteral("clothoid-road"));
  EXPECT_EQ(first.cacheKey(), second.cacheKey());
}

TEST(Icons, UnknownNameFallsBackToThemeWithoutCrashing) {
  // Offscreen platforms typically have no icon theme, so the fallback may be
  // a null icon — the contract is a safe, cached return, not a pixmap.
  const QIcon icon = Icons::get(QStringLiteral("rm-test-does-not-exist"));
  const QIcon again = Icons::get(QStringLiteral("rm-test-does-not-exist"));
  EXPECT_EQ(icon.isNull(), again.isNull());
}

TEST(Icons, ClearCacheKeepsLookupsWorking) {
  Icons::clear_cache();
  const QIcon icon = Icons::get(QStringLiteral("clothoid-road"));
  EXPECT_FALSE(icon.isNull());
}

} // namespace
