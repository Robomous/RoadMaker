#pragma once

#include <QIcon>
#include <QString>

namespace roadmaker::editor {

// Palette-aware icon loader for the bundled monochrome SVG set
// (editor/resources/resources.qrc, addressed as :/icons/<name>.svg).
// SVGs use stroke="currentColor" and are tinted to the active palette's
// WindowText color (Normal and Disabled) at load, so one asset serves light
// and dark themes. Names without a bundled asset fall back to
// QIcon::fromTheme(name).
//
// Icons are cached per name; call clear_cache() when the application palette
// changes (QEvent::ApplicationPaletteChange) so tints are recomputed.
class Icons {
public:
  Icons() = delete;

  [[nodiscard]] static QIcon get(const QString& name);
  static void clear_cache();
};

} // namespace roadmaker::editor
