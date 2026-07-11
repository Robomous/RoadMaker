#pragma once

// Delete tool (issue #11, docs/design/m2/02_editing_tools.md §7). A click
// deletes the picked road through edit::delete_road — no confirmation, undo
// is the confirmation; the kernel command carries the full referential
// closure (junction connections + orphaned connecting roads). Multi-object
// deletion lives on the Select tool's Delete key. Headless by construction:
// ToolEvent in, commands out.

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

class DeleteTool : public Tool {
  Q_OBJECT

public:
  explicit DeleteTool(Document& document, QObject* parent = nullptr);

  void activate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;

private:
  Document& document_;
};

} // namespace roadmaker::editor
