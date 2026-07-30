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

#include "panels/asset_import_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace roadmaker::editor {

namespace {

/// "brick_wall_02" -> "Brick Wall 02": a first guess at a display name, so the
/// common case is one keystroke (Enter) rather than typing a name out.
QString label_from_filename(const QString& base) {
  QString label = base;
  label.replace(QLatin1Char('_'), QLatin1Char(' '));
  label.replace(QLatin1Char('-'), QLatin1Char(' '));
  QStringList words = label.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  for (QString& word : words) {
    word[0] = word[0].toUpper();
  }
  return words.join(QLatin1Char(' '));
}

} // namespace

AssetImportDialog::AssetImportDialog(const std::filesystem::path& source,
                                     const QStringList& categories,
                                     QWidget* parent)
    : QDialog(parent), source_(source) {
  setWindowTitle(tr("Import Asset"));

  const QFileInfo info(QString::fromStdString(source.string()));
  auto* form = new QFormLayout;
  form->addRow(tr("File"), new QLabel(info.fileName(), this));

  name_edit_ = new QLineEdit(label_from_filename(info.completeBaseName()), this);
  form->addRow(tr("&Name"), name_edit_);

  category_combo_ = new QComboBox(this);
  category_combo_->setEditable(true);
  category_combo_->addItems(categories);
  if (categories.isEmpty()) {
    category_combo_->addItem(tr("Materials"));
  }
  form->addRow(tr("&Category"), category_combo_);

  license_edit_ = new QLineEdit(this);
  license_edit_->setPlaceholderText(tr("e.g. CC0, CC-BY 4.0, or your own work"));
  form->addRow(tr("&Licence"), license_edit_);

  attestation_ = new QCheckBox(tr("I have the right to use this file in this project"), this);
  // Not pre-checked: a pre-checked attestation is not an attestation. The user
  // has to make the claim, which is the whole reason this dialog exists.
  attestation_->setChecked(false);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  ok_button_ = buttons->button(QDialogButtonBox::Ok);
  ok_button_->setText(tr("Import"));

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(attestation_);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(name_edit_, &QLineEdit::textChanged, this, &AssetImportDialog::update_ok_enabled);
  connect(attestation_, &QCheckBox::toggled, this, &AssetImportDialog::update_ok_enabled);
  update_ok_enabled();

  name_edit_->setFocus();
  name_edit_->selectAll();
}

void AssetImportDialog::update_ok_enabled() {
  // A name that slugifies to nothing would be refused by the importer anyway;
  // catching it here means the user is told before they commit rather than after.
  const bool nameable = !asset_slug(name_edit_->text()).isEmpty();
  ok_button_->setEnabled(nameable && attestation_->isChecked());
}

AssetImportRequest AssetImportDialog::request() const {
  AssetImportRequest request;
  request.source = source_;
  request.label = name_edit_->text().trimmed();
  request.category = category_combo_->currentText().trimmed();
  request.license = license_edit_->text().trimmed();
  return request;
}

} // namespace roadmaker::editor
