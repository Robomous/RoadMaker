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

// Crosswalk & Stop Line tool (p3-s3, issue #222): headless ToolEvent sequences
// place a crosswalk + stop line on a junction approach and assert the network,
// the ONE-undo-unit semantics, and the chevron placement affordance. This is
// the primary GW-5 s5 / GW-2 s10 proof under QT_QPA_PLATFORM=offscreen.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <stdexcept>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/crosswalk_stop_line_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::RoadEnd;
using roadmaker::Waypoint;

ToolEvent event_at(double x, double y) {
  ToolEvent event;
  event.buttons = Qt::LeftButton;
  event.world_x = x;
  event.world_y = y;
  return event;
}

/// A 3-arm signalized junction built through the Document, so the placement
/// commands stack on a real undo stack (west/east/south approaches).
struct Scene {
  Document document;
  int base_count = 0;

  Scene() {
    const auto arm = [&](Waypoint a, Waypoint b) {
      if (!document.push_command(
              roadmaker::edit::create_road({a, b}, roadmaker::LaneProfile::two_lane_rural(), ""))) {
        throw std::runtime_error("arm setup failed");
      }
    };
    arm(Waypoint{-40.0, 0.0}, Waypoint{-6.0, 0.0}); // road "1", End at (-6, 0)
    arm(Waypoint{40.0, 0.0}, Waypoint{6.0, 0.0});   // road "2", End at (6, 0)
    arm(Waypoint{0.0, -40.0}, Waypoint{0.0, -6.0}); // road "3", End at (0, -6)
    const std::vector<RoadEnd> ends{{document.network().find_road("1"), ContactPoint::End},
                                    {document.network().find_road("2"), ContactPoint::End},
                                    {document.network().find_road("3"), ContactPoint::End}};
    if (!document.push_command(roadmaker::edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction setup failed");
    }
    base_count = document.undo_stack()->count();
  }

  int objects_on(const char* odr_road) const {
    const RoadId road = document.network().find_road(odr_road);
    int count = 0;
    document.network().for_each_object([&](ObjectId, const Object& object) {
      if (object.road == road) {
        ++count;
      }
    });
    return count;
  }
};

TEST(CrosswalkStopLineTool, ClickPlacesCrosswalkAndStopLineAsOneUndoUnit) {
  Scene scene;
  SelectionModel selection(scene.document);
  CrosswalkStopLineTool tool(scene.document, selection);
  tool.activate();

  // Click the west approach, 10 m out from the junction.
  ASSERT_TRUE(tool.mouse_release(event_at(-10.0, 0.0)));

  // ONE object lands — the crosswalk. The stop line is derived (p4-s3, #318),
  // so it was already there; the release only authors its record.
  const RoadId west = scene.document.network().find_road("1");
  int crosswalks = 0;
  int stop_line_objects = 0;
  scene.document.network().for_each_object([&](ObjectId, const Object& object) {
    if (object.road != west) {
      return;
    }
    if (object.type == ObjectType::Crosswalk) {
      ++crosswalks;
    } else if (object.type_str == "roadMark" && object.subtype == "signalLines") {
      ++stop_line_objects;
    }
  });
  EXPECT_EQ(crosswalks, 1);
  EXPECT_EQ(stop_line_objects, 0) << "the stop line is derived, never an arena object";

  // The arm's stop line now carries the record, linked back to the crosswalk.
  const JunctionId junction = scene.document.network().find_junction("1");
  const RoadEnd arm{.road = west, .contact = ContactPoint::End};
  const auto* record = [&]() -> const StopLine* {
    for (const StopLine& line : scene.document.network().junction(junction)->stoplines) {
      if (line.arm == arm) {
        return &line;
      }
    }
    return nullptr;
  }();
  ASSERT_NE(record, nullptr);
  EXPECT_FALSE(record->crosswalk_odr_id.empty()) << "the line records its crosswalk";

  // Exactly ONE undo entry (the macro) for the whole placement.
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count + 1);

  // The placed crosswalk is selected.
  ASSERT_EQ(selection.selected_objects().size(), 1U);

  // One Ctrl+Z removes the crosswalk AND the stop-line record together.
  EXPECT_EQ(scene.objects_on("1"), 1);
  scene.document.undo_stack()->undo();
  EXPECT_EQ(scene.objects_on("1"), 0);
  EXPECT_TRUE(scene.document.network().junction(junction)->stoplines.empty());
}

TEST(CrosswalkStopLineTool, UndoRedoRoundTripsByteIdentically) {
  Scene scene;
  SelectionModel selection(scene.document);
  CrosswalkStopLineTool tool(scene.document, selection);
  tool.activate();

  ASSERT_TRUE(tool.mouse_release(event_at(-10.0, 0.0)));
  const auto placed = roadmaker::write_xodr(scene.document.network(), "crosswalk tool");
  ASSERT_TRUE(placed.has_value());

  scene.document.undo_stack()->undo();
  scene.document.undo_stack()->redo();
  const auto restored = roadmaker::write_xodr(scene.document.network(), "crosswalk tool");
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*placed, *restored);
}

TEST(CrosswalkStopLineTool, HoverOverAnApproachEmitsAChevron) {
  Scene scene;
  SelectionModel selection(scene.document);
  CrosswalkStopLineTool tool(scene.document, selection);
  tool.activate();

  EXPECT_FALSE(tool.mouse_move(event_at(-10.0, 0.0))); // hover never consumes
  const auto preview = tool.preview();
  // The chevron is two world segments (2 × 6 doubles) plus the anchor handle.
  EXPECT_FALSE(preview.line_positions.empty());
  EXPECT_EQ(preview.line_positions.size(), 12U);
  EXPECT_FALSE(preview.handles.empty());
}

TEST(CrosswalkStopLineTool, HoverOffAnyApproachShowsNoChevron) {
  Scene scene;
  SelectionModel selection(scene.document);
  CrosswalkStopLineTool tool(scene.document, selection);
  tool.activate();

  EXPECT_FALSE(tool.mouse_move(event_at(0.0, 50.0))); // far from the junction
  const auto preview = tool.preview();
  EXPECT_TRUE(preview.line_positions.empty());
  EXPECT_TRUE(preview.handles.empty());
}

TEST(CrosswalkStopLineTool, ClickInOpenSpacePlacesNothingAndToasts) {
  Scene scene;
  SelectionModel selection(scene.document);
  CrosswalkStopLineTool tool(scene.document, selection);
  tool.activate();

  QSignalSpy toast(&tool, &Tool::toast_requested);
  ASSERT_TRUE(tool.mouse_release(event_at(0.0, 50.0)));
  EXPECT_EQ(scene.document.undo_stack()->count(), scene.base_count); // nothing pushed
  EXPECT_GE(toast.count(), 1);                                       // rejected, not silent
}

} // namespace
} // namespace roadmaker::editor
