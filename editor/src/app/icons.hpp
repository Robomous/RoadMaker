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

  // Full-colour application icon (the coral robot), assembled from the
  // multi-size PNGs under :/branding/. Used for QApplication::setWindowIcon —
  // the window/taskbar icon on every platform (macOS Dock/Finder come from the
  // bundled .icns instead). Not palette-tinted; cached after first build.
  [[nodiscard]] static QIcon app_icon();
};

} // namespace roadmaker::editor
