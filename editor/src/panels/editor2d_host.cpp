#include "panels/editor2d_host.hpp"

#include <QVBoxLayout>

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

} // namespace roadmaker::editor
