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

// Prop Point tool (p6-s4, issue #238): headless ToolEvent sequences place a prop
// on/beside a road and assert the network, the ONE-undo-entry semantics,
// byte-identical undo, the drag = one move command path, the off-road no-op, the
// wrong-asset toast, selection, and the hover ghost. Runs under
// QT_QPA_PLATFORM=offscreen.

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <optional>
#include <stdexcept>
#include <string>

#include "document/document.hpp"
#include "document/library_manifest.hpp"
#include "document/selection_model.hpp"
#include "tools/prop_point_tool.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::Waypoint;

LibraryItem pine_item() {
  LibraryItem item;
  item.key = "prop.tree.pine";
  item.label = "Pine tree";
  item.kind = LibraryItem::Kind::Tree;
  item.model = "tree_pine";
  return item;
}

ToolEvent event_at(double x, double y) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = x;
  event.world_y = y;
  return event;
}

/// A single straight two-lane road built through the Document, so placements
/// stack on a real undo stack.
struct Scene {
  Document document;
  int base_count = 0;

  Scene() {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{-40.0, 0.0}, Waypoint{40.0, 0.0}},
                                         roadmaker::LaneProfile::two_lane_rural(),
                                         ""))) {
      throw std::runtime_error("road setup failed");
    }
    base_count = document.undo_stack()->count();
  }

  int object_count() const {
    int count = 0;
    document.network().for_each_object([&](ObjectId, const Object&) { ++count; });
    return count;
  }

  std::optional<ObjectId> first_object() const {
    std::optional<ObjectId> found;
    document.network().for_each_object([&](ObjectId id, const Object&) {
      if (!found.has_value()) {
        found = id;
      }
    });
    return found;
  }
};

TEST(PropPointTool, ClickPlacesOnePropAsOneUndoEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));

  EXPECT_EQ(scene.object_count(), 1);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);
  EXPECT_EQ(selection.selected_objects().size(), 1U);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.object_count(), 0);
}

TEST(PropPointTool, ScaledPlacementPersistsDimsAsLayerZero) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] {
    LibraryItem item = pine_item();
    item.default_scale = 2.0;
    return item;
  });
  tool.activate();
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  ASSERT_EQ(scene.object_count(), 1);

  const props::PropModel* model = props::model("tree_pine");
  ASSERT_NE(model, nullptr);
  const auto id = scene.first_object();
  ASSERT_TRUE(id.has_value());
  const Object* placed = scene.document.network().object(*id);
  ASSERT_NE(placed, nullptr);
  ASSERT_TRUE(placed->height.has_value());
  EXPECT_DOUBLE_EQ(*placed->height, model->height * 2.0);

  // Write -> read keeps the scaled dims; persistence is Layer 0 (plain
  // @height/@radius, no rm:/userData in the <objects> block).
  const auto xml = roadmaker::write_xodr(scene.document.network(), "prop scale");
  ASSERT_TRUE(xml.has_value());
  const auto reparsed = roadmaker::parse_xodr(*xml, "prop scale");
  ASSERT_TRUE(reparsed.has_value());
  int reparsed_count = 0;
  std::optional<double> reparsed_height;
  reparsed->network.for_each_object([&](ObjectId, const Object& obj) {
    ++reparsed_count;
    reparsed_height = obj.height;
  });
  EXPECT_EQ(reparsed_count, 1);
  ASSERT_TRUE(reparsed_height.has_value());
  EXPECT_DOUBLE_EQ(*reparsed_height, model->height * 2.0);

  // Scope the no-userData assertion to the <objects> slice — a road always
  // carries its own rm:waypoints userData, so a whole-file check would be wrong.
  const std::string& doc = *xml;
  const std::string::size_type begin = doc.find("<objects>");
  const std::string::size_type end = doc.find("</objects>");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const std::string objects = doc.substr(begin, end - begin);
  EXPECT_EQ(objects.find("userData"), std::string::npos);
  EXPECT_EQ(objects.find("rm:"), std::string::npos);
}

TEST(PropPointTool, RepeatedClicksPlaceSeparateProps) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  for (double x : {-10.0, 0.0, 10.0}) {
    ASSERT_TRUE(tool.mouse_press(event_at(x, -1.75)));
    ASSERT_TRUE(tool.mouse_release(event_at(x, -1.75)));
  }
  EXPECT_EQ(scene.object_count(), 3);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 3);
}

TEST(PropPointTool, UndoRoundTripsByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  const auto before = roadmaker::write_xodr(scene.document.network(), "prop point");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  scene.document.undo_stack()->undo();

  const auto after = roadmaker::write_xodr(scene.document.network(), "prop point");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*before, *after);
}

TEST(PropPointTool, DragMovesPropAsExactlyOneCommand) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  const auto object = scene.first_object();
  ASSERT_TRUE(object.has_value());
  const RoadId road = scene.document.network().object(*object)->road;
  const int after_place = scene.document.undo_stack()->count();

  ToolEvent press = event_at(0.0, -1.75);
  press.pick = PickHit{.road = road, .object = *object};
  ASSERT_TRUE(tool.mouse_press(press));
  ASSERT_TRUE(tool.mouse_move(event_at(12.0, -1.75))); // beyond the drag tolerance
  ASSERT_TRUE(tool.mouse_release(event_at(12.0, -1.75)));

  // Exactly ONE more undo entry for the whole drag (no mergeWith).
  EXPECT_EQ(scene.document.undo_stack()->count(), after_place + 1);
}

TEST(PropPointTool, EscCancelsDragWithoutEntry) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  const auto object = scene.first_object();
  ASSERT_TRUE(object.has_value());
  const RoadId road = scene.document.network().object(*object)->road;
  const int after_place = scene.document.undo_stack()->count();

  ToolEvent press = event_at(0.0, -1.75);
  press.pick = PickHit{.road = road, .object = *object};
  ASSERT_TRUE(tool.mouse_press(press));
  ASSERT_TRUE(tool.mouse_move(event_at(12.0, -1.75)));
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_EQ(scene.document.undo_stack()->count(), after_place); // nothing committed
}

TEST(PropPointTool, OffRoadPlacesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  ASSERT_TRUE(tool.mouse_press(event_at(0.0, 80.0)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, 80.0)));
  EXPECT_EQ(scene.object_count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
}

TEST(PropPointTool, WrongAssetToastsAndPlacesNothing) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return LibraryItem{}; }); // Kind::Unknown
  tool.activate();

  QSignalSpy toast(&tool, &Tool::toast_requested);
  ASSERT_TRUE(tool.mouse_press(event_at(0.0, -1.75)));
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, -1.75)));
  EXPECT_EQ(scene.object_count(), 0);
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count);
  EXPECT_GE(toast.count(), 1);
}

TEST(PropPointTool, HoverShowsGhost) {
  Scene scene;
  SelectionModel selection(scene.document);
  PropPointTool tool(scene.document, selection);
  tool.set_params_provider([] { return pine_item(); });
  tool.activate();

  // A plain hover over the road arms the ghost footprint.
  EXPECT_FALSE(tool.mouse_move(event_at(0.0, -1.75)));
  EXPECT_FALSE(tool.preview().empty());
}

} // namespace
} // namespace roadmaker::editor
