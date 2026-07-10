#pragma once

// Table model over the parser/validator diagnostics: severity | rule id |
// location | message. The rule id is best-effort extracted from the message
// (the kernel Diagnostic has no rule-id field until M2).

#include <QAbstractTableModel>

#include "document/document.hpp"

namespace roadmaker::editor {

class DiagnosticsModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column : int { kSeverity = 0, kRuleId, kLocation, kMessage, kColumnCount };

  explicit DiagnosticsModel(const Document& document, QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

  /// The diagnostic behind a row (nullptr when out of range) — used by the
  /// panel to resolve double-clicks into a selection.
  [[nodiscard]] const Diagnostic* diagnostic_at(int row) const;

private:
  const Document& document_;
};

} // namespace roadmaker::editor
