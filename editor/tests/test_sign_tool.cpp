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

// Sign tool (p4-s9, issue #230): headless ToolEvent sequences place a road sign
// as ONE undo entry, defaulting to a StVO 310 text plate when the Library offers
// no sign, placing a selected sign asset otherwise, committing a drag as exactly
// one command, and leaving the document byte-identical when Esc cancels.
//
// Runs under QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/signal.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QUndoStack>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/sign_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::LaneProfile;
using roadmaker::RoadId;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// One straight road along +x, authored through the command layer.
struct Scene {
  Document document;
  SelectionModel selection{document};
  std::string base_xodr;

  Scene() {
    if (!document.push_command(roadmaker::edit::create_road(
            {{0.0, 0.0}, {120.0, 0.0}}, LaneProfile::two_lane_default(), ""))) {
      throw std::runtime_error("create_road failed");
    }
    base_xodr = xodr(document);
  }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

LibraryItem sign_asset(const char* tag) {
  LibraryItem item;
  item.kind = LibraryItem::Kind::Signal;
  item.signal = QString::fromLatin1(tag);
  return item;
}

const Signal* only_signal(const Document& document) {
  const Signal* found = nullptr;
  document.network().for_each_signal([&](SignalId, const Signal& s) { found = &s; });
  return found;
}

} // namespace

TEST(SignTool, ClickPlacesTextSignWithDefaultText) {
  Scene scene;
  SignTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return LibraryItem{}; }); // no Library sign
  tool.activate();
  const int base = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(60.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(60.0, 3.0)));

  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1) << "one undo entry";
  ASSERT_EQ(scene.document.network().signal_count(), 1U);
  const Signal* placed = only_signal(scene.document);
  ASSERT_NE(placed, nullptr);
  EXPECT_EQ(placed->type, "310");
  EXPECT_EQ(placed->text, "City");
  EXPECT_TRUE(scene.selection.primary().signal.is_valid()) << "the placed sign is selected";
}

TEST(SignTool, DefaultsToTextSignWhenLibraryHasNoSign) {
  Scene scene;
  SignTool tool(scene.document, scene.selection);
  // A non-sign item (or empty) must still place a usable text sign.
  tool.set_params_provider([] { return LibraryItem{}; });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(at(60.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(60.0, 3.0)));
  ASSERT_EQ(scene.document.network().signal_count(), 1U);
  EXPECT_EQ(only_signal(scene.document)->type, "310");
}

TEST(SignTool, PlacesTheSelectedSignAsset) {
  Scene scene;
  SignTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return sign_asset("sign_stop"); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(at(60.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(60.0, 3.0)));
  ASSERT_EQ(scene.document.network().signal_count(), 1U);
  EXPECT_EQ(only_signal(scene.document)->type, "206"); // StVO 206 STOP
}

TEST(SignTool, DragCommitsExactlyOneCommand) {
  Scene scene;
  SignTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return LibraryItem{}; });
  tool.activate();
  const int base = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(40.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(50.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(scene.document.preview_active()) << "the drag runs in a preview session";
  EXPECT_TRUE(tool.mouse_move(at(70.0, 4.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(70.0, 4.0)));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1)
      << "a drag is ONE command on release, never one per frame";
  EXPECT_EQ(scene.document.network().signal_count(), 1U);
}

TEST(SignTool, EscCancelLeavesXodrByteIdentical) {
  Scene scene;
  SignTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return LibraryItem{}; });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(at(40.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(60.0, 3.0, Qt::LeftButton))); // opens a preview
  EXPECT_TRUE(scene.document.preview_active());
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.network().signal_count(), 0U);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr) << "cancel restores the document";
}

} // namespace roadmaker::editor
