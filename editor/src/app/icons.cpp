// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "app/icons.hpp"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSize>
#include <QSvgRenderer>
#include <initializer_list>
#include <mutex>

// Q_INIT_RESOURCE must be invoked from the global namespace. The editor lib
// is static: without this explicit reference the linker is free to drop the
// generated resource object file, leaving :/icons/ empty at runtime.
static void rm_init_icon_resources() {
  Q_INIT_RESOURCE(resources);
}

namespace roadmaker::editor {
namespace {

void ensure_resources() {
  static std::once_flag resources_once;
  std::call_once(resources_once, rm_init_icon_resources);
}

QHash<QString, QIcon>& icon_cache() {
  static QHash<QString, QIcon> cache;
  return cache;
}

QPixmap tinted_pixmap(QSvgRenderer& renderer, int size, qreal dpr, const QColor& color) {
  QPixmap pixmap(static_cast<int>(size * dpr), static_cast<int>(size * dpr));
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  renderer.render(&painter, QRectF(0.0, 0.0, size, size));
  painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
  painter.fillRect(QRectF(0.0, 0.0, size, size), color);
  return pixmap;
}

} // namespace

QIcon Icons::get(const QString& name) {
  ensure_resources();

  auto& cache = icon_cache();
  if (const auto it = cache.constFind(name); it != cache.constEnd()) {
    return *it;
  }

  QSvgRenderer renderer(QStringLiteral(":/icons/%1.svg").arg(name));
  if (!renderer.isValid()) {
    const QIcon themed = QIcon::fromTheme(name);
    cache.insert(name, themed);
    return themed;
  }

  const QPalette palette = QGuiApplication::palette();
  const QColor normal = palette.color(QPalette::Normal, QPalette::WindowText);
  const QColor disabled = palette.color(QPalette::Disabled, QPalette::WindowText);

  QIcon icon;
  // 28 joins the ladder for the labeled main toolbar (ui-design.md sizes).
  for (const int size : {16, 20, 24, 28, 32}) {
    for (const qreal dpr : {1.0, 2.0}) {
      icon.addPixmap(tinted_pixmap(renderer, size, dpr, normal), QIcon::Normal);
      icon.addPixmap(tinted_pixmap(renderer, size, dpr, disabled), QIcon::Disabled);
    }
  }
  cache.insert(name, icon);
  return icon;
}

QIcon Icons::app_icon() {
  ensure_resources();

  static const QIcon icon = [] {
    QIcon built;
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
      built.addFile(QStringLiteral(":/branding/icon_%1.png").arg(size), QSize(size, size));
    }
    return built;
  }();
  return icon;
}

void Icons::clear_cache() {
  icon_cache().clear();
}

} // namespace roadmaker::editor
