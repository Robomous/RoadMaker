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

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <memory>

#include "tools/tool.hpp"
#include "tools/tool_manager.hpp"

namespace {

using roadmaker::editor::PreviewGeometry;
using roadmaker::editor::Tool;
using roadmaker::editor::ToolEvent;
using roadmaker::editor::ToolId;
using roadmaker::editor::ToolManager;

// Records lifecycle calls so activation ordering is observable.
class RecordingTool final : public Tool {
public:
  explicit RecordingTool(std::vector<QString>& log, QString name)
      : log_(log), name_(std::move(name)) {}

  void activate() override { log_.push_back(name_ + ":activate"); }

  void deactivate() override { log_.push_back(name_ + ":deactivate"); }

private:
  std::vector<QString>& log_;
  QString name_;
};

TEST(Tool, DefaultHandlersConsumeNothing) {
  Tool tool;
  const ToolEvent event{};
  EXPECT_FALSE(tool.mouse_press(event));
  EXPECT_FALSE(tool.mouse_move(event));
  EXPECT_FALSE(tool.mouse_release(event));
  EXPECT_FALSE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_TRUE(tool.preview().empty());
}

TEST(ToolManager, StartsWithNoActiveTool) {
  ToolManager manager;
  EXPECT_EQ(manager.active(), nullptr);
  EXPECT_FALSE(manager.active_id().has_value());
}

TEST(ToolManager, SetActiveActivatesAndSignals) {
  ToolManager manager;
  std::vector<QString> log;
  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "select"));

  QSignalSpy spy(&manager, &ToolManager::active_changed);
  manager.set_active(ToolId::Select);

  ASSERT_EQ(spy.count(), 1);
  ASSERT_TRUE(manager.active_id().has_value());
  EXPECT_EQ(*manager.active_id(), ToolId::Select);
  ASSERT_EQ(log.size(), 1u);
  EXPECT_EQ(log[0], QStringLiteral("select:activate"));
}

TEST(ToolManager, SwitchDeactivatesPreviousBeforeActivatingNext) {
  ToolManager manager;
  std::vector<QString> log;
  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "select"));
  manager.register_tool(ToolId::CreateRoad, std::make_unique<RecordingTool>(log, "road"));

  manager.set_active(ToolId::Select);
  manager.set_active(ToolId::CreateRoad);

  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[1], QStringLiteral("select:deactivate"));
  EXPECT_EQ(log[2], QStringLiteral("road:activate"));
}

TEST(ToolManager, ReactivatingCurrentToolIsANoop) {
  ToolManager manager;
  std::vector<QString> log;
  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "select"));

  manager.set_active(ToolId::Select);
  QSignalSpy spy(&manager, &ToolManager::active_changed);
  manager.set_active(ToolId::Select);

  EXPECT_EQ(spy.count(), 0);
  EXPECT_EQ(log.size(), 1u);
}

TEST(ToolManager, UnknownIdIsIgnored) {
  ToolManager manager;
  std::vector<QString> log;
  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "select"));
  manager.set_active(ToolId::Select);

  QSignalSpy spy(&manager, &ToolManager::active_changed);
  manager.set_active(ToolId::Delete); // never registered

  EXPECT_EQ(spy.count(), 0);
  ASSERT_TRUE(manager.active_id().has_value());
  EXPECT_EQ(*manager.active_id(), ToolId::Select);
}

TEST(ToolManager, ReplacingActiveToolDeactivatesIt) {
  ToolManager manager;
  std::vector<QString> log;
  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "old"));
  manager.set_active(ToolId::Select);

  manager.register_tool(ToolId::Select, std::make_unique<RecordingTool>(log, "new"));

  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[1], QStringLiteral("old:deactivate"));
  EXPECT_EQ(manager.active(), nullptr);
}

} // namespace
