#include "panels/editor2d_host.hpp"

#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/network.hpp"

#include <QVBoxLayout>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

ProfileEditorPage::ProfileEditorPage(Document& document, SelectionModel& selection, QWidget* parent)
    : panel_(new ProfilePanel(document, selection, parent)) {}

QString ProfileEditorPage::title() const {
  return QObject::tr("Vertical Profile");
}

QWidget* ProfileEditorPage::widget() {
  return panel_;
}

bool ProfileEditorPage::relevant(const SelectionModel& selection) const {
  return selection.primary().road.is_valid();
}

WidthEditorPage::WidthEditorPage(Document& document, SelectionModel& selection, QWidget* parent)
    : panel_(new WidthPanel(document, selection, parent)) {}

QString WidthEditorPage::title() const {
  return QObject::tr("Lane Width");
}

QWidget* WidthEditorPage::widget() {
  return panel_;
}

bool WidthEditorPage::relevant(const SelectionModel& selection) const {
  return selection.primary().lane.is_valid();
}

SignalPhaseEditorPage::SignalPhaseEditorPage(Document& document,
                                             SelectionModel& selection,
                                             QWidget* parent)
    : document_(document), panel_(new PhasePanel(document, selection, parent)) {}

QString SignalPhaseEditorPage::title() const {
  // This exact literal is matched by Editor2DHost::show_page from MainWindow —
  // keep it identical in the action handler and the Signal tool activation.
  return QObject::tr("Signal Phases");
}

QWidget* SignalPhaseEditorPage::widget() {
  return panel_;
}

bool SignalPhaseEditorPage::relevant(const SelectionModel& selection) const {
  const JunctionId junction = selection.primary().junction;
  if (!junction.is_valid() || document_.network().junction(junction) == nullptr) {
    return false;
  }
  for (const JunctionApproachInfo& approach : junction_signals(document_.network(), junction)) {
    if (approach.dynamic || !approach.controller_odr_ids.empty()) {
      return true; // a light-controlled junction has a cycle to time
    }
  }
  return false;
}

Editor2DHost::Editor2DHost(const SelectionModel& selection, QWidget* parent)
    : QWidget(parent), selection_(selection), tabs_(new QTabWidget(this)) {
  setObjectName(QStringLiteral("editor2d_host"));
  tabs_->setObjectName(QStringLiteral("editor2d_tabs"));
  tabs_->setDocumentMode(true);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(tabs_);

  // The host follows the selection to surface the right editor; the PAGES
  // subscribe on their own for their content, exactly as they did before they
  // were hosted.
  connect(
      &selection_, &SelectionModel::selection_changed, this, &Editor2DHost::raise_relevant_page);
}

void Editor2DHost::register_page(std::unique_ptr<Editor2DPage> page) {
  QWidget* widget = page->widget();
  const QString title = page->title();
  pages_.push_back(std::move(page));
  tabs_->addTab(widget, title);
}

void Editor2DHost::raise_relevant_page() {
  const int current = tabs_->currentIndex();
  if (current >= 0 && static_cast<std::size_t>(current) < pages_.size() &&
      pages_[static_cast<std::size_t>(current)]->relevant(selection_)) {
    return; // already on a useful tab — don't yank it away mid-edit
  }
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i]->relevant(selection_)) {
      tabs_->setCurrentIndex(static_cast<int>(i));
      return;
    }
  }
  // Nothing relevant: leave the tab where it is rather than pick arbitrarily.
}

bool Editor2DHost::show_page(const QString& title) {
  for (int i = 0; i < tabs_->count(); ++i) {
    if (tabs_->tabText(i) == title) {
      tabs_->setCurrentIndex(i);
      return true;
    }
  }
  return false;
}

} // namespace roadmaker::editor
