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
#include <QMessageBox>
#include <filesystem>
#include <optional>
#include <system_error>

#include "help/help_locator.hpp"
#include "help/manual_locator.hpp"

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

bool HelpBrowser::open_manual_page(const QString& slug) {
  const std::optional<std::filesystem::path> index = manual_index();
  if (index) {
    const std::optional<std::filesystem::path> page =
        manual_page_for(index->parent_path(), slug.toStdString());
    std::error_code ec;
    if (page && std::filesystem::exists(*page, ec) && !ec) {
      return QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(page->string())));
    }
    // The manual is here but this page is not: still better to land the reader on
    // its front page than to do nothing.
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(index->string())));
  }
  return false;
}

void HelpBrowser::on_anchor_clicked(const QUrl& url) {
  if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
    QDesktopServices::openUrl(url);
    return;
  }

  // A reference page's bridge into the full guide (helpc::kManualScheme). The
  // manual is a sibling of this collection in the install tree, so the target is
  // only knowable at runtime — which is why the compiler emits a scheme rather
  // than a path.
  if (url.scheme() == QLatin1String("rmmanual")) {
    // QUrl parses `rmmanual:tutorials/x` as an opaque path, not a host.
    const QString slug = url.path();
    if (!open_manual_page(slug)) {
      // No bundled manual — the normal state of a developer build. Say where the
      // guide is rather than failing silently or showing a raw error.
      auto* box = new QMessageBox(QMessageBox::Information,
                                  tr("Full Guide Not Bundled"),
                                  tr("This build does not include the full HTML manual. "
                                     "It is available online at:<br><a href=\"%1\">%1</a>")
                                      .arg(QLatin1String(kGithubUserGuideUrl)),
                                  QMessageBox::Ok,
                                  this);
      box->setTextFormat(Qt::RichText);
      box->setAttribute(Qt::WA_DeleteOnClose);
      box->open(); // non-blocking: never stalls a headless run
    }
    return;
  }

  setSource(url);
}

} // namespace roadmaker::editor::help
