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

#include "document/diagnostics_model.hpp"

#include "app/icons.hpp"

namespace roadmaker::editor {

namespace {

QString severity_icon_name(Severity severity) {
  switch (severity) {
  case Severity::Info:
    return QStringLiteral("info");
  case Severity::Warning:
    return QStringLiteral("triangle-alert");
  case Severity::Error:
    return QStringLiteral("octagon-x");
  }
  return {};
}

QString severity_label(Severity severity) {
  switch (severity) {
  case Severity::Info:
    return QStringLiteral("Info");
  case Severity::Warning:
    return QStringLiteral("Warning");
  case Severity::Error:
    return QStringLiteral("Error");
  }
  return {};
}

} // namespace

DiagnosticsModel::DiagnosticsModel(const Document& document, QObject* parent)
    : QAbstractTableModel(parent), document_(document) {
  connect(&document_, &Document::diagnostics_changed, this, [this] {
    beginResetModel();
    endResetModel();
  });
}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(document_.diagnostics().size());
}

int DiagnosticsModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant DiagnosticsModel::data(const QModelIndex& index, int role) const {
  const Diagnostic* diagnostic = diagnostic_at(index.row());
  if (diagnostic == nullptr) {
    return {};
  }
  if (role == Qt::DecorationRole && index.column() == kSeverity) {
    return Icons::get(severity_icon_name(diagnostic->severity));
  }
  if (role != Qt::DisplayRole) {
    return {};
  }
  switch (index.column()) {
  case kSeverity:
    return severity_label(diagnostic->severity);
  case kRuleId:
    return QString::fromStdString(diagnostic->rule_id);
  case kLocation:
    return QString::fromStdString(diagnostic->location);
  case kMessage:
    return QString::fromStdString(diagnostic->message);
  default:
    return {};
  }
}

QVariant DiagnosticsModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  switch (section) {
  case kSeverity:
    return tr("Severity");
  case kRuleId:
    return tr("Rule id");
  case kLocation:
    return tr("Location");
  case kMessage:
    return tr("Message");
  default:
    return {};
  }
}

const Diagnostic* DiagnosticsModel::diagnostic_at(int row) const {
  const auto& diagnostics = document_.diagnostics();
  if (row < 0 || static_cast<std::size_t>(row) >= diagnostics.size()) {
    return nullptr;
  }
  return &diagnostics[static_cast<std::size_t>(row)];
}

} // namespace roadmaker::editor
