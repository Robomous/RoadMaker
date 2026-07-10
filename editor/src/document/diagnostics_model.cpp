#include "document/diagnostics_model.hpp"

namespace roadmaker::editor {

namespace {

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
  if (diagnostic == nullptr || role != Qt::DisplayRole) {
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
