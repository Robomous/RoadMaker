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

// The rich-text pane of the help window. Resolves qthelp:// resources (pages,
// stylesheet, images) out of the QHelpEngine, and hands http(s) links to the
// system browser instead of trying to render them.

#include <QTextBrowser>
#include <QUrl>
#include <QVariant>

class QHelpEngineCore;

namespace roadmaker::editor::help {

class HelpBrowser : public QTextBrowser {
  Q_OBJECT

public:
  explicit HelpBrowser(QHelpEngineCore& engine, QWidget* parent = nullptr);

  /// Public seam over the protected loadResource override (tested directly).
  [[nodiscard]] QVariant resource(int type, const QUrl& name);

protected:
  QVariant loadResource(int type, const QUrl& name) override;

private:
  void on_anchor_clicked(const QUrl& url);

  QHelpEngineCore& engine_;
};

} // namespace roadmaker::editor::help
