// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
