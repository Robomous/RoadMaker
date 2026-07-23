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

#include "help/help_viewer.hpp"

#include <QHelpContentWidget>
#include <QHelpEngine>
#include <QHelpIndexWidget>
#include <QHelpLink>
#include <QHelpSearchEngine>
#include <QHelpSearchQueryWidget>
#include <QHelpSearchResultWidget>
#include <QLabel>
#include <QMessageBox>
#include <QSplitter>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "help/help_browser.hpp"
#include "help/help_locator.hpp"

namespace roadmaker::editor::help {

namespace {

QString resolved_collection() {
  const std::optional<std::filesystem::path> path = writable_collection();
  if (!path) {
    return {};
  }
  return QString::fromStdString(path->string());
}

} // namespace

HelpViewer::HelpViewer(QWidget* parent) : HelpViewer(resolved_collection(), parent) {}

HelpViewer::HelpViewer(const QString& collection_file, QWidget* parent) : QWidget(parent) {
  setWindowTitle(tr("RoadMaker User Guide"));
  resize(1000, 700);

  if (!collection_file.isEmpty()) {
    engine_ = new QHelpEngine(collection_file, this);
    if (engine_->setupData() &&
        engine_->registeredDocumentations().contains(QLatin1String(kNamespace))) {
      available_ = true;
    }
  }

  build_ui();
}

QString HelpViewer::page_url(const QString& slug) {
  return QStringLiteral("qthelp://%1/%2/%3.html")
      .arg(QLatin1String(kNamespace), QLatin1String(kFolder), slug);
}

QString HelpViewer::current_page() const {
  return browser_ != nullptr ? browser_->source().toString() : QString();
}

void HelpViewer::build_ui() {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  if (!available_) {
    // A minimal placeholder — open_page() is the real fallback path.
    auto* note = new QLabel(tr("The bundled user guide is unavailable. It is online at "
                               "<a href=\"%1\">%1</a>.")
                                .arg(QLatin1String(kGithubUserGuideUrl)),
                            this);
    note->setOpenExternalLinks(true);
    note->setWordWrap(true);
    note->setMargin(24);
    layout->addWidget(note);
    return;
  }

  auto* splitter = new QSplitter(Qt::Horizontal, this);

  auto* tabs = new QTabWidget(splitter);
  tabs->addTab(engine_->contentWidget(), tr("Contents"));
  tabs->addTab(engine_->indexWidget(), tr("Index"));

  auto* search = new QWidget(tabs);
  auto* search_layout = new QVBoxLayout(search);
  search_layout->setContentsMargins(0, 0, 0, 0);
  QHelpSearchEngine* search_engine = engine_->searchEngine();
  search_layout->addWidget(search_engine->queryWidget());
  search_layout->addWidget(search_engine->resultWidget());
  tabs->addTab(search, tr("Search"));

  browser_ = new HelpBrowser(*engine_, splitter);
  splitter->addWidget(tabs);
  splitter->addWidget(browser_);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({260, 740});
  layout->addWidget(splitter);

  connect(engine_->contentWidget(),
          &QHelpContentWidget::linkActivated,
          browser_,
          [this](const QUrl& url) { browser_->setSource(url); });
  // QHelpIndexWidget::linkActivated is deprecated in Qt 6.8 (MSVC flags it under
  // /WX); documentActivated carries a QHelpLink whose .url is the target.
  connect(engine_->indexWidget(),
          &QHelpIndexWidget::documentActivated,
          browser_,
          [this](const QHelpLink& document, const QString&) { browser_->setSource(document.url); });
  connect(search_engine->resultWidget(),
          &QHelpSearchResultWidget::requestShowLink,
          browser_,
          [this](const QUrl& url) { browser_->setSource(url); });
  connect(search_engine->queryWidget(), &QHelpSearchQueryWidget::search, this, [search_engine] {
    search_engine->search(search_engine->queryWidget()->searchInput());
  });

  search_engine->reindexDocumentation();
}

void HelpViewer::open_page(const QString& slug) {
  if (!available_ || browser_ == nullptr) {
    auto* box = new QMessageBox(QMessageBox::Information,
                                tr("User Guide Unavailable"),
                                tr("The bundled user guide could not be opened. "
                                   "It is available online at:<br><a href=\"%1\">%1</a>")
                                    .arg(QLatin1String(kGithubUserGuideUrl)),
                                QMessageBox::Ok,
                                this);
    box->setTextFormat(Qt::RichText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open(); // non-blocking: never stalls a headless run
    return;
  }
  browser_->setSource(QUrl(page_url(slug)));
}

} // namespace roadmaker::editor::help
