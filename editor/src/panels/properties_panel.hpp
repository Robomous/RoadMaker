#pragma once

// Read-only properties of the selected entity (M1.5 scope: display only).
// Rebuilt from the kernel on every selection change; no state of its own.

#include <QFormLayout>
#include <QLabel>
#include <QWidget>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

class PropertiesPanel : public QWidget {
  Q_OBJECT

public:
  PropertiesPanel(const Document& document,
                  const SelectionModel& selection,
                  QWidget* parent = nullptr);

private:
  void refresh();
  void add_row(const QString& label, const QString& value);
  void clear_rows();

  const Document& document_;
  const SelectionModel& selection_;
  QFormLayout* form_;
  QLabel* placeholder_;
};

} // namespace roadmaker::editor
