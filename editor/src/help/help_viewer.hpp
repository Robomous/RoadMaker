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

// The in-app user-guide window: a Contents/Index/Search sidebar beside the
// rich-text browser, served from the shipped Qt Help collection. Reachable from
// Help ▸ User Guide and F1.

#include <QString>
#include <QWidget>

class QHelpEngine;

namespace roadmaker::editor::help {

class HelpBrowser;

class HelpViewer : public QWidget {
  Q_OBJECT

public:
  /// Resolves the collection via writable_collection().
  explicit HelpViewer(QWidget* parent = nullptr);

  /// Explicit collection file — the test seam. An empty or unusable path
  /// leaves the viewer unavailable().
  HelpViewer(const QString& collection_file, QWidget* parent);

  /// True when the shipped collection loaded and our namespace is registered.
  [[nodiscard]] bool available() const { return available_; }

  /// Navigate to `<slug>.html`. When unavailable, shows a non-blocking dialog
  /// pointing at the guide on GitHub instead.
  void open_page(const QString& slug);

  /// The qthelp:// URL for a page slug.
  [[nodiscard]] static QString page_url(const QString& slug);

  /// The URL of the page currently shown, or empty when unavailable.
  [[nodiscard]] QString current_page() const;

private:
  void build_ui();

  QHelpEngine* engine_ = nullptr;
  HelpBrowser* browser_ = nullptr;
  bool available_ = false;
};

} // namespace roadmaker::editor::help
