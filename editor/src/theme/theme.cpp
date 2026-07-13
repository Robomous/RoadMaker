#include "theme/theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <algorithm>
#include <utility>
#include <vector>

namespace roadmaker::editor {

namespace {

std::array<float, 3> rgb(const QColor& color) {
  return {static_cast<float>(color.redF()),
          static_cast<float>(color.greenF()),
          static_cast<float>(color.blueF())};
}

std::array<float, 4> rgba(const QColor& color, float alpha) {
  return {static_cast<float>(color.redF()),
          static_cast<float>(color.greenF()),
          static_cast<float>(color.blueF()),
          alpha};
}

/// "rgba(r, g, b, a)" for QSS — alpha in 0..255.
QString qss_rgba(const QColor& color, int alpha) {
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(alpha);
}

/// One string template, tokens injected by name. Longest names are replaced
/// first so shared prefixes (@border vs @borderStrong) cannot mis-substitute.
QString build_qss(const Theme& t) {
  QString qss = QStringLiteral(R"qss(
/* Generated from theme tokens (docs/standards/ui-design.md) — do not add
   hardcoded colors here or anywhere else in the editor. */

QMainWindow::separator { background: @bg0; width: 3px; height: 3px; }

QToolBar { background: @bg2; border: none; border-bottom: 1px solid @border;
           padding: 4px 8px; spacing: 2px; }
QToolBar::separator { background: @border; width: 1px; margin: 6px 8px; }
QToolBar QToolButton { background: transparent; color: @textSec; border: none;
                       border-radius: 4px; padding: 4px 8px; }
QToolBar QToolButton:hover { background: @hoverOverlay; color: @text; }
QToolBar QToolButton:pressed { background: @pressedOverlay; }
QToolBar QToolButton:checked { background: @accentSoft; color: @accent; }
QToolBar QToolButton:disabled { color: @textDis; }

QMenuBar { background: @bg1; color: @text; }
QMenuBar::item { padding: 4px 10px; border-radius: 4px; }
QMenuBar::item:selected { background: @accentSoft; }
QMenu { background: @bg1; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; color: @text; }
QMenu::item:selected { background: @accentSoft; }
QMenu::item:disabled { color: @textDis; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }

QDockWidget { color: @textSec; font-weight: 600; }
QDockWidget::title { background: @bg2; padding: 6px 10px; text-align: left;
                     border-bottom: 1px solid @border; }
QTabBar::tab { background: transparent; color: @textSec; padding: 6px 14px;
               border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover { color: @text; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }

QStatusBar { background: @bg2; border-top: 1px solid @border; color: @textSec; }
QStatusBar QLabel { color: @textSec; }
QStatusBar::item { border: none; }

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
  background: @bgInput; border: 1px solid @border; border-radius: 4px;
  padding: 3px 6px; color: @text;
  selection-background-color: @accent; selection-color: @onAccent; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
  border: 1px solid @accent; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled,
QDoubleSpinBox:disabled { color: @textDis; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView { background: @bg1; border: 1px solid @border;
  selection-background-color: @accentSoft; selection-color: @text; }

QTreeView, QListView, QTableView {
  background: @bgInput; alternate-background-color: @bg1;
  border: none; color: @text; }
QTreeView::item, QListView::item { padding: 2px; }
QTreeView::item:hover, QListView::item:hover { background: @hoverOverlay; }
QTreeView::item:selected, QListView::item:selected {
  background: @accentSoft; color: @text; }
QHeaderView::section { background: @bg2; color: @textSec; border: none;
  border-bottom: 1px solid @border; padding: 4px 8px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: @borderStrong; border-radius: 5px; }
QScrollBar::handle:vertical { min-height: 24px; }
QScrollBar::handle:horizontal { min-width: 24px; }
QScrollBar::handle:hover { background: @textDis; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QPushButton { background: @bg2; color: @text; border: 1px solid @border;
              border-radius: 4px; padding: 5px 14px; }
QPushButton:hover { border-color: @borderStrong; background: @hoverOverlay; }
QPushButton:pressed { background: @pressedOverlay; }
QPushButton:default { background: @accent; color: @onAccent; border: none;
                      font-weight: 600; }
QPushButton:default:hover { background: @accentHover; }
QPushButton:disabled { color: @textDis; border-color: @border; }

QToolTip { background: @bg2; color: @text; border: 1px solid @borderStrong;
           padding: 4px 6px; }

QLabel#toolOptionCaption { color: @textSec; font-weight: 600; }
QLabel#toolOptionHint { color: @textSec; }

#welcomeRoot { background: @bg0; }
QPushButton#welcomePrimary { background: @accent; color: @onAccent;
  border: none; font-weight: 600; font-size: 14px; }
QPushButton#welcomePrimary:hover { background: @accentHover; }
#welcomeHero { font-size: 30px; font-weight: 700; color: @text; }
#welcomeTagline { color: @textSec; font-size: 14px; }
#welcomeVersion { color: @textDis; }
QLabel#welcomeSection { color: @textSec; font-size: 12px; font-weight: 600;
                        letter-spacing: 1px; }
#welcomeRoot QListWidget { background: transparent; border: none; }
#welcomeRoot QListWidget::item { background: @bg1; border: 1px solid @border;
  border-radius: 8px; padding: 8px; color: @text; }
#welcomeRoot QListWidget::item:hover { border-color: @accent;
  background: @bg2; }
#welcomeRoot QListWidget::item:selected { background: @accentSoft;
  border-color: @accent; color: @text; }
)qss");

  std::vector<std::pair<QString, QString>> tokens = {
      {QStringLiteral("@bg0"), t.bg0.name()},
      {QStringLiteral("@bg1"), t.bg1.name()},
      {QStringLiteral("@bg2"), t.bg2.name()},
      {QStringLiteral("@bgInput"), t.bg_input.name()},
      {QStringLiteral("@borderStrong"), t.border_strong.name()},
      {QStringLiteral("@border"), t.border.name()},
      {QStringLiteral("@textSec"), t.text_secondary.name()},
      {QStringLiteral("@textDis"), t.text_disabled.name()},
      {QStringLiteral("@text"), t.text_primary.name()},
      {QStringLiteral("@accentHover"), t.accent_hover.name()},
      {QStringLiteral("@accentSoft"), qss_rgba(t.accent, 46)},
      {QStringLiteral("@accent"), t.accent.name()},
      {QStringLiteral("@onAccent"), t.on_accent.name()},
      {QStringLiteral("@hoverOverlay"), qss_rgba(t.text_primary, 18)},
      {QStringLiteral("@pressedOverlay"), qss_rgba(t.text_primary, 30)},
  };
  std::ranges::sort(tokens,
                    [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  for (const auto& [token, value] : tokens) {
    qss.replace(token, value);
  }
  return qss;
}

QPalette build_palette(const Theme& t) {
  QPalette palette;
  palette.setColor(QPalette::Window, t.bg1);
  palette.setColor(QPalette::WindowText, t.text_primary);
  palette.setColor(QPalette::Base, t.bg_input);
  palette.setColor(QPalette::AlternateBase, t.bg1);
  palette.setColor(QPalette::Text, t.text_primary);
  palette.setColor(QPalette::PlaceholderText, t.text_disabled);
  palette.setColor(QPalette::Button, t.bg2);
  palette.setColor(QPalette::ButtonText, t.text_primary);
  palette.setColor(QPalette::Highlight, t.accent);
  palette.setColor(QPalette::HighlightedText, t.on_accent);
  palette.setColor(QPalette::ToolTipBase, t.bg2);
  palette.setColor(QPalette::ToolTipText, t.text_primary);
  palette.setColor(QPalette::Link, t.accent);
  palette.setColor(QPalette::LinkVisited, t.accent);
  palette.setColor(QPalette::BrightText, t.error);
  // Fusion frame shades.
  palette.setColor(QPalette::Light, t.bg2);
  palette.setColor(QPalette::Midlight, t.border);
  palette.setColor(QPalette::Mid, t.border);
  palette.setColor(QPalette::Dark, t.bg0);
  palette.setColor(QPalette::Shadow, t.bg0);

  for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
    palette.setColor(QPalette::Disabled, role, t.text_disabled);
  }
  palette.setColor(QPalette::Disabled, QPalette::Highlight, t.border_strong);
  palette.setColor(QPalette::Disabled, QPalette::HighlightedText, t.text_disabled);
  return palette;
}

const Theme* g_current = nullptr;

} // namespace

BackdropColors Theme::backdrop() const {
  BackdropColors colors;
  colors.sky_top = rgb(sky_top);
  colors.sky_horizon = rgb(sky_horizon);
  colors.grid_major = rgba(grid_major, 0.9F);
  colors.grid_minor = rgba(grid_minor, 0.55F);
  // Origin axes keep the DCC convention (X reddish, Y greenish) across all
  // palettes — muted so they read as orientation, not content.
  colors.axis_x = {0.75F, 0.35F, 0.32F};
  colors.axis_y = {0.35F, 0.65F, 0.36F};
  // Hover/selection emphasis follows the accent token so alternate palettes
  // (and future retints) drive the viewport highlight too.
  colors.highlight = rgb(accent);
  return colors;
}

namespace theme {

const Theme& graphite_amber() {
  static const Theme t{
      .name = QStringLiteral("graphite-amber"),
      .display_name = QStringLiteral("Graphite / Amber"),
      .bg0 = QColor(0x13, 0x14, 0x17),
      .bg1 = QColor(0x1b, 0x1d, 0x21),
      .bg2 = QColor(0x22, 0x25, 0x2a),
      .bg_input = QColor(0x10, 0x11, 0x14),
      .border = QColor(0x2e, 0x32, 0x38),
      .border_strong = QColor(0x43, 0x49, 0x51),
      .text_primary = QColor(0xe8, 0xea, 0xed),
      .text_secondary = QColor(0xa7, 0xad, 0xb5),
      .text_disabled = QColor(0x5f, 0x66, 0x6e),
      .accent = QColor(0xf5, 0xa6, 0x23),
      .accent_hover = QColor(0xff, 0xb8, 0x4d),
      .on_accent = QColor(0x1f, 0x16, 0x00),
      .warning = QColor(0xe3, 0xb3, 0x41),
      .error = QColor(0xe5, 0x53, 0x4b),
      .success = QColor(0x57, 0xab, 0x5a),
      .sky_top = QColor(0x1d, 0x20, 0x26),
      .sky_horizon = QColor(0x3a, 0x41, 0x49),
      .grid_major = QColor(0x53, 0x5a, 0x63),
      .grid_minor = QColor(0x33, 0x38, 0x3f),
  };
  return t;
}

const Theme& slate_cyan() {
  static const Theme t{
      .name = QStringLiteral("slate-cyan"),
      .display_name = QStringLiteral("Slate / Cyan"),
      .bg0 = QColor(0x0f, 0x13, 0x19),
      .bg1 = QColor(0x17, 0x1d, 0x26),
      .bg2 = QColor(0x1e, 0x26, 0x31),
      .bg_input = QColor(0x0d, 0x11, 0x17),
      .border = QColor(0x2b, 0x36, 0x44),
      .border_strong = QColor(0x40, 0x4f, 0x62),
      .text_primary = QColor(0xe6, 0xed, 0xf3),
      .text_secondary = QColor(0x9f, 0xb0, 0xc0),
      .text_disabled = QColor(0x5b, 0x68, 0x76),
      .accent = QColor(0x35, 0xc0, 0xd8),
      .accent_hover = QColor(0x5c, 0xd3, 0xe8),
      .on_accent = QColor(0x00, 0x1b, 0x20),
      .warning = QColor(0xe3, 0xb3, 0x41),
      .error = QColor(0xf0, 0x55, 0x5d),
      .success = QColor(0x4c, 0xc3, 0x8a),
      .sky_top = QColor(0x18, 0x22, 0x2d),
      .sky_horizon = QColor(0x35, 0x48, 0x5c),
      .grid_major = QColor(0x4c, 0x5f, 0x74),
      .grid_minor = QColor(0x2c, 0x3a, 0x49),
  };
  return t;
}

const Theme& warm_signal() {
  static const Theme t{
      .name = QStringLiteral("warm-signal"),
      .display_name = QStringLiteral("Warm Dark / Signal Yellow"),
      .bg0 = QColor(0x17, 0x15, 0x12),
      .bg1 = QColor(0x1f, 0x1c, 0x18),
      .bg2 = QColor(0x27, 0x23, 0x1e),
      .bg_input = QColor(0x14, 0x12, 0x0f),
      .border = QColor(0x3a, 0x34, 0x2c),
      .border_strong = QColor(0x52, 0x4a, 0x3f),
      .text_primary = QColor(0xec, 0xe7, 0xde),
      .text_secondary = QColor(0xb3, 0xaa, 0x9c),
      .text_disabled = QColor(0x6e, 0x67, 0x5c),
      .accent = QColor(0xe8, 0xc2, 0x27),
      .accent_hover = QColor(0xf6, 0xd3, 0x4f),
      .on_accent = QColor(0x20, 0x1a, 0x00),
      .warning = QColor(0xd9, 0xa0, 0x3f),
      .error = QColor(0xe0, 0x56, 0x4a),
      .success = QColor(0x7a, 0xa8, 0x5c),
      .sky_top = QColor(0x22, 0x1e, 0x19),
      .sky_horizon = QColor(0x45, 0x3c, 0x30),
      .grid_major = QColor(0x5c, 0x53, 0x45),
      .grid_minor = QColor(0x36, 0x30, 0x28),
  };
  return t;
}

const Theme& default_theme() {
  // Maintainer pick from the Phase 0 three-mockup checkpoint (2026-07-12):
  // graphite-amber (ui-design.md "Default palette").
  return graphite_amber();
}

QStringList available() {
  return {graphite_amber().name, slate_cyan().name, warm_signal().name};
}

const Theme* by_name(const QString& name) {
  for (const Theme* t : {&graphite_amber(), &slate_cyan(), &warm_signal()}) {
    if (t->name == name) {
      return t;
    }
  }
  return nullptr;
}

void apply(QApplication& app, const Theme& theme) {
  g_current = by_name(theme.name) != nullptr ? by_name(theme.name) : &default_theme();
  QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  app.setPalette(build_palette(theme));
  app.setStyleSheet(build_qss(theme));
}

const Theme& current() {
  return g_current != nullptr ? *g_current : default_theme();
}

} // namespace theme

} // namespace roadmaker::editor
