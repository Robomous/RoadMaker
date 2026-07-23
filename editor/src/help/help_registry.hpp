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

// The context-help map: which committed user-guide page answers "help me with
// what I'm looking at right now". F1 routes through context_page() — the
// focused dock's page, else the active tool's page, else the guide index.
//
// Like the shortcut map (app/shortcut_registry.hpp) this is ONE static table,
// not a virtual on each tool/panel: panels share no base class, and the
// coverage test (test_help_registry.cpp) must enumerate every ToolId and dock
// name headlessly, without constructing a single widget. Adding a tool or a
// dock without a row is a gap the coverage test turns into a red build.

#include <QString>
#include <QStringView>
#include <optional>
#include <span>

#include "tools/tool.hpp" // ToolId

namespace roadmaker::editor::help {

/// One tool -> guide-page-slug row. `slug` is a committed page under
/// docs/user-guide/ (no ".md", no path) — the same slug the help collection
/// keys its keyword id on.
struct ToolPage {
  ToolId id;
  const char* slug;
};

/// One dock -> guide-page-slug row. `dock` is the QDockWidget objectName set in
/// main_window.cpp (e.g. "dock.scene"); `slug` is a committed page.
struct DockPage {
  const char* dock;
  const char* slug;
};

/// Every tool, one row each (asserted complete in the coverage test).
[[nodiscard]] std::span<const ToolPage> tool_table();

/// Every dock, one row each (asserted against the canonical dock-name list).
[[nodiscard]] std::span<const DockPage> dock_table();

/// The page slug for `id`, or an empty string when unmapped (never happens once
/// the coverage test passes — every ToolId has a row).
[[nodiscard]] QString page_for_tool(ToolId id);

/// The page slug for a dock objectName, or an empty string when the name is not
/// a known dock (an empty/unknown name from a focus that is not inside a dock).
[[nodiscard]] QString page_for_dock(QStringView dock_name);

/// The context page for F1. Priority: the focused dock's page, then the active
/// tool's page, then "index". `focused_dock` is empty when focus is not inside
/// a dock; `active_tool` is nullopt when no tool is active.
[[nodiscard]] QString context_page(std::optional<ToolId> active_tool, QStringView focused_dock);

} // namespace roadmaker::editor::help
