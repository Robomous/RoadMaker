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

  [[nodiscard]] QString instruction() const override;

private:
  Document& document_;
};

} // namespace roadmaker::editor
