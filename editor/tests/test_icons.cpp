#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPalette>
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

TEST(Icons, AppIconIsBundledAndMultiSize) {
  // The full-colour application icon (window/taskbar icon) is assembled from
  // the :/branding/ PNGs; a non-null pixmap at a small and a large size proves
  // the raster set is embedded and QIcon selected an image.
  const QIcon icon = Icons::app_icon();
  ASSERT_FALSE(icon.isNull());
  EXPECT_FALSE(icon.pixmap(16).isNull());
  EXPECT_FALSE(icon.pixmap(256).isNull());
}

TEST(Icons, EveryMappedIconIsBundled) {
  // The full docs/design/m2/05_assets.md §1 mapping table. A non-null pixmap
  // proves the bundled SVG rendered (the fromTheme fallback yields a null
  // icon on the offscreen platform).
  const char* const names[] = {// Lucide 1.24.0
                               "box",
                               "circle-plus",
                               "file-output",
                               "file-plus",
                               "folder-open",
                               "info",
                               "magnet",
                               "mountain",
                               "mouse-pointer-2",
                               "move",
                               "octagon-x",
                               "redo-2",
                               "rotate-ccw",
                               "save",
                               "scan",
                               "trash-2",
                               "trees",
                               "triangle-alert",
                               "undo-2",
                               "waypoints",
                               // custom, drawn on the Lucide grid
                               "clothoid-road",
                               "junction-connect",
                               "lane-section",
                               "template-highway",
                               "template-rural",
                               "template-urban"};
  for (const char* name : names) {
    SCOPED_TRACE(name);
    const QIcon icon = Icons::get(QLatin1String(name));
    ASSERT_FALSE(icon.isNull());
    EXPECT_FALSE(icon.pixmap(16).isNull());
  }
}

/// The tint color of the first fully opaque pixel — CompositionMode_SourceIn
/// stamps the palette color over the whole alpha mask, so any opaque pixel
/// carries exactly the tint.
QColor sampled_tint(const QIcon& icon) {
  const QImage image = icon.pixmap(32).toImage().convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      if (color.alpha() == 255) {
        return color;
      }
    }
  }
  return {};
}

TEST(Icons, PaletteChangeRetintsAfterCacheClear) {
  const QPalette original = QGuiApplication::palette();

  QPalette red = original;
  red.setColor(QPalette::Normal, QPalette::WindowText, QColor(200, 30, 40));
  QGuiApplication::setPalette(red);
  Icons::clear_cache();
  const QColor first = sampled_tint(Icons::get(QStringLiteral("save")));
  EXPECT_EQ(first, QColor(200, 30, 40));

  QPalette blue = original;
  blue.setColor(QPalette::Normal, QPalette::WindowText, QColor(30, 60, 220));
  QGuiApplication::setPalette(blue);
  Icons::clear_cache();
  const QColor second = sampled_tint(Icons::get(QStringLiteral("save")));
  EXPECT_EQ(second, QColor(30, 60, 220));

  QGuiApplication::setPalette(original);
  Icons::clear_cache();
}

} // namespace
