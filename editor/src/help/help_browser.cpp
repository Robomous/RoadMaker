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

#include "help/help_browser.hpp"

#include <QDesktopServices>
#include <QHelpEngineCore>

namespace roadmaker::editor::help {

HelpBrowser::HelpBrowser(QHelpEngineCore& engine, QWidget* parent)
    : QTextBrowser(parent), engine_(engine) {
  // We route navigation ourselves: qthelp:// stays in the pane, http(s) opens
  // externally. setOpenLinks(false) hands every click to anchorClicked.
  setOpenLinks(false);
  connect(this, &QTextBrowser::anchorClicked, this, &HelpBrowser::on_anchor_clicked);
}

QVariant HelpBrowser::loadResource(int type, const QUrl& name) {
  if (name.scheme() == QLatin1String("qthelp")) {
    return QVariant(engine_.fileData(name));
  }
  return QTextBrowser::loadResource(type, name);
}

QVariant HelpBrowser::resource(int type, const QUrl& name) {
  return loadResource(type, name);
}

void HelpBrowser::on_anchor_clicked(const QUrl& url) {
  if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
    QDesktopServices::openUrl(url);
    return;
  }
  setSource(url);
}

} // namespace roadmaker::editor::help
