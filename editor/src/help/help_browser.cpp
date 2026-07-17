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
