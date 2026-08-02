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
// stylesheet, images) out of the QHelpEngine, hands http(s) links to the system
// browser instead of trying to render them, and resolves the `rmmanual:` bridge
// links the help compiler emits (helpc::kManualScheme) against the packaged HTML
// manual — also in the system browser, per ADR-0009.

#include <QString>
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

  /// Open one page of the packaged manual in the system browser. False when no
  /// manual shipped with this build, which is what selects the pointer at the
  /// online docs. Public so the decision is testable without a click.
  [[nodiscard]] bool open_manual_page(const QString& slug);

protected:
  QVariant loadResource(int type, const QUrl& name) override;

private:
  void on_anchor_clicked(const QUrl& url);

  QHelpEngineCore& engine_;
};

} // namespace roadmaker::editor::help
