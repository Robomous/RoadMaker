// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Signal tool (p4-s7, issue #228): headless ToolEvent sequences target a
// junction, apply and clear a signalization template, and place a single head
// on a road — asserting that the two commands are ONE undo entry each and that
// undo restores the network bytes, that the tool DISABLES rather than fails on
// every precondition the kernel refuses (span/virtual junction, re-applying the
// applied template), that a placement drag is exactly one command, that Esc and
// deactivate() cancel a live session, and that the overlay links each head to
// the movements it gates.
//
// The overlay assertions are STRUCTURAL on purpose: the editor's screenshot
// mode instantiates no tool, so a tool overlay cannot be captured without new
// plumbing (the p4-s6 precedent). These tests are the evidence.
//
// Runs under QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_signals.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "tools/signal_tool.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionApproachInfo;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::edit::SignalizeTemplate;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four roomy arms meeting at the origin (they stop 20 m short, the tight-
/// junction clamp trap from p4-s1), joined into a junction.
struct Scene {
  Document document;
  SelectionModel selection{document};
  JunctionId junction;
  int base_count = 0;
  std::string base_xodr;

  Scene() {
    const std::array<RoadEnd, 4> ends{RoadEnd{make(-80.0, 0.0, -20.0, 0.0, "1"), ContactPoint::End},
                                      RoadEnd{make(80.0, 0.0, 20.0, 0.0, "2"), ContactPoint::End},
                                      RoadEnd{make(0.0, -80.0, 0.0, -20.0, "3"), ContactPoint::End},
                                      RoadEnd{make(0.0, 80.0, 0.0, 20.0, "4"), ContactPoint::End}};
    if (!document.push_command(roadmaker::edit::create_junction(document.network(), ends))) {
      throw std::runtime_error("junction create failed");
    }
    document.network().for_each_junction(
        [&](JunctionId id, const roadmaker::Junction&) { junction = id; });
    base_count = document.undo_stack()->index();
    base_xodr = xodr(document);
  }

  RoadId make(double x0, double y0, double x1, double y1, const char* odr) {
    if (!document.push_command(
            roadmaker::edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                                         LaneProfile::two_lane_default(),
                                         odr))) {
      throw std::runtime_error("road create failed");
    }
    return document.network().find_road(odr);
  }

  void select_junction() {
    selection.select(SelectionEntry{.junction = junction}, SelectMode::Replace);
  }

  [[nodiscard]] std::vector<JunctionApproachInfo> approaches() const {
    return junction_signals(document.network(), junction);
  }

  [[nodiscard]] const roadmaker::Junction* record() const {
    return document.network().junction(junction);
  }
};

ToolEvent at(double x, double y, Qt::MouseButtons buttons = Qt::NoButton) {
  ToolEvent event;
  event.world_x = x;
  event.world_y = y;
  event.buttons = buttons;
  return event;
}

/// The Library's built-in traffic-light asset, as the tool's params provider
/// hands it over.
LibraryItem light_item() {
  LibraryItem item;
  item.kind = LibraryItem::Kind::Signal;
  item.signal = QStringLiteral("light");
  item.label = QStringLiteral("Traffic light");
  return item;
}

} // namespace

TEST(SignalTool, TemplateTokensRoundTripThroughTheirPersistedSpelling) {
  // The enum, the token table and the persisted rm:signal value are one mapping;
  // a template added to one and not the other must not silently persist wrong.
  for (const SignalizeTemplate tmpl : {SignalizeTemplate::FourWayProtectedLeft,
                                       SignalizeTemplate::TwoPhase,
                                       SignalizeTemplate::AllWayStop,
                                       SignalizeTemplate::TwoWayStop}) {
    const std::string_view token = signalize_template_token(tmpl);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(signalize_template_from_token(token), tmpl);
  }
  EXPECT_FALSE(signalize_template_from_token("no_such_template").has_value());
}

TEST(SignalTool, TargetIsGatedOnAJunctionSelection) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();

  EXPECT_FALSE(tool.inspected_junction().is_valid());
  EXPECT_TRUE(tool.approaches().empty());
  EXPECT_FALSE(tool.can_signalize());
  EXPECT_FALSE(tool.can_clear());
  EXPECT_FALSE(tool.signalize_blocker().isEmpty()) << "an unavailable command must say why";

  QSignalSpy spy(&tool, &SignalTool::signalization_changed);
  scene.select_junction();
  EXPECT_EQ(tool.inspected_junction(), scene.junction);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(tool.approaches().size(), 4U) << "one approach per arm of the cross";
  EXPECT_TRUE(tool.can_signalize());
  EXPECT_FALSE(tool.can_clear()) << "nothing is signalized yet";
  // Targeting a junction authors nothing.
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SignalTool, ClickingAJunctionFloorTargetsItAndSelectsIt) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();

  ToolEvent event = at(0.0, 0.0, Qt::LeftButton);
  PickHit hit;
  hit.junction = scene.junction;
  event.pick = hit;
  EXPECT_TRUE(tool.mouse_press(event));
  EXPECT_EQ(tool.inspected_junction(), scene.junction);
  EXPECT_EQ(scene.selection.primary().junction, scene.junction);
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
}

TEST(SignalTool, SignalizeIsOneUndoEntryAndUndoRestoresTheBytes) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();

  ASSERT_TRUE(tool.signalize());
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1)
      << "a whole signalization is ONE undo step";
  ASSERT_NE(scene.record(), nullptr);
  EXPECT_EQ(scene.record()->signalization.tmpl, signalize_template_token(tool.pending_template()));
  EXPECT_FALSE(scene.record()->junction_controllers.empty())
      << "the default template is dynamic, so it forms a synchronization group";
  EXPECT_TRUE(tool.applied_template().has_value());
  EXPECT_TRUE(tool.can_clear());

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SignalTool, ReApplyingTheAppliedTemplateIsDisabledRatherThanFailing) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  ASSERT_TRUE(tool.signalize());
  const int after = scene.document.undo_stack()->index();

  // The factory REFUSES a no-op (the round-trip oracle forbids one), so the UI
  // must never offer it.
  EXPECT_FALSE(tool.can_signalize());
  EXPECT_FALSE(tool.signalize_blocker().isEmpty());
  EXPECT_FALSE(tool.signalize());
  EXPECT_EQ(scene.document.undo_stack()->index(), after);

  // A DIFFERENT template is offered again, and switching replaces the previous
  // generation rather than stacking on it.
  tool.set_pending_template(SignalizeTemplate::AllWayStop);
  EXPECT_TRUE(tool.can_signalize());
  ASSERT_TRUE(tool.signalize());
  EXPECT_EQ(scene.record()->signalization.tmpl,
            signalize_template_token(SignalizeTemplate::AllWayStop));
  EXPECT_TRUE(scene.record()->junction_controllers.empty())
      << "an all-way stop has no phases, so it creates no controllers";
}

TEST(SignalTool, ClearRemovesWhatSignalizeAuthored) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  ASSERT_TRUE(tool.signalize());
  ASSERT_TRUE(tool.can_clear());

  ASSERT_TRUE(tool.clear());
  EXPECT_TRUE(scene.record()->signalization.tmpl.empty());
  EXPECT_TRUE(scene.record()->junction_controllers.empty());
  EXPECT_TRUE(scene.record()->signal_mounts.empty());
  EXPECT_FALSE(tool.can_clear()) << "clearing twice would be a no-op";
  // Signalize + clear round-trips the file back to where it started.
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SignalTool, ASpanJunctionCannotBeSignalized) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();

  // A virtual junction over one arm road: ASAM says it "shall not have
  // controllers and therefore no traffic lights".
  const RoadId road = scene.document.network().find_road("1");
  const std::array<roadmaker::SpanArm, 1> spans{
      roadmaker::SpanArm{.road = road, .s_start = 5.0, .s_end = 25.0}};
  ASSERT_TRUE(
      scene.document.push_command(edit::create_span_junction(scene.document.network(), spans)));
  JunctionId span_junction;
  scene.document.network().for_each_junction([&](JunctionId id, const roadmaker::Junction& j) {
    if (!j.spans.empty()) {
      span_junction = id;
    }
  });
  ASSERT_TRUE(span_junction.is_valid());

  scene.selection.select(SelectionEntry{.junction = span_junction}, SelectMode::Replace);
  EXPECT_EQ(tool.inspected_junction(), span_junction);
  EXPECT_FALSE(tool.can_signalize());
  EXPECT_TRUE(tool.signalize_blocker().contains(QStringLiteral("span")))
      << "the refusal must name the reason, not just fail";
  EXPECT_FALSE(tool.can_clear());
}

TEST(SignalTool, ClickingARoadPlacesOneSignalAsOneUndoEntry) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return light_item(); });
  tool.activate();
  const int base = scene.document.undo_stack()->index();

  // Beside the west arm, well outside the junction.
  ASSERT_TRUE(tool.mouse_press(at(-60.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(-60.0, 2.0)));
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1);
  EXPECT_EQ(scene.document.network().signal_count(), 1U);
  EXPECT_TRUE(scene.selection.primary().signal.is_valid()) << "the placed head is selected";

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SignalTool, PlacementDragCommitsExactlyOneCommand) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return light_item(); });
  tool.activate();
  const int base = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(-60.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(-50.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(scene.document.preview_active()) << "the drag runs in a preview session";
  EXPECT_TRUE(tool.mouse_move(at(-40.0, 3.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(-40.0, 3.0)));

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1)
      << "a drag is ONE command on release, never one per frame";
  EXPECT_EQ(scene.document.network().signal_count(), 1U);
}

TEST(SignalTool, EscapeAndDeactivateCancelALivePlacement) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return light_item(); });
  tool.activate();
  const int base = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(-60.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(-50.0, 2.0, Qt::LeftButton)));
  ASSERT_TRUE(scene.document.preview_active());
  EXPECT_TRUE(tool.key_press(Qt::Key_Escape, Qt::NoModifier));
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_EQ(scene.document.network().signal_count(), 0U);

  // And a tool switch mid-drag cancels the same way (a leaked session is what
  // the soak driver's first invariant catches).
  ASSERT_TRUE(tool.mouse_press(at(-60.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_move(at(-50.0, 2.0, Qt::LeftButton)));
  ASSERT_TRUE(scene.document.preview_active());
  tool.deactivate();
  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(SignalTool, PlacingWithoutASignalAssetAuthorsNothing) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate(); // no params provider: the Library has nothing selected
  const int base = scene.document.undo_stack()->index();

  ASSERT_TRUE(tool.mouse_press(at(-60.0, 2.0, Qt::LeftButton)));
  EXPECT_TRUE(tool.mouse_release(at(-60.0, 2.0)));
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
  EXPECT_EQ(scene.document.network().signal_count(), 0U);
}

TEST(SignalTool, OverlayLinksEveryHeadToTheMovementsItGates) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.activate();
  scene.select_junction();
  EXPECT_TRUE(tool.preview().empty()) << "an unsignalized junction has nothing to draw";

  ASSERT_TRUE(tool.signalize());
  const PreviewGeometry geometry = tool.preview();

  // One handle per placed head.
  std::size_t heads = 0;
  for (const JunctionApproachInfo& approach : tool.approaches()) {
    heads += approach.signal_ids.size();
    EXPECT_FALSE(approach.gated.empty()) << "every arm of a cross feeds turns";
  }
  EXPECT_GT(heads, 0U);
  EXPECT_EQ(geometry.handles.size(), heads);

  // Dotted leaders: one segment (six doubles) per head→gated-movement pair.
  EXPECT_FALSE(geometry.dashed_line_positions.empty())
      << "each head draws a leader to every movement it gates";
  EXPECT_EQ(geometry.dashed_line_positions.size() % 6, 0U);

  // Controller groups are drawn as a chain linking their heads. The default
  // template is dynamic, so there is at least one group with two heads.
  EXPECT_FALSE(geometry.line_positions.empty()) << "controller groups draw their chains";
}

TEST(SignalTool, ASignalPickSelectsTheHeadAndLeavesTheTargetAlone) {
  Scene scene;
  SignalTool tool(scene.document, scene.selection);
  tool.set_params_provider([] { return light_item(); });
  tool.activate();
  scene.select_junction();
  ASSERT_TRUE(tool.signalize());

  SignalId head;
  RoadId head_road;
  scene.document.network().for_each_signal([&](SignalId id, const roadmaker::Signal& signal) {
    if (!head.is_valid()) {
      head = id;
      head_road = signal.road;
    }
  });
  ASSERT_TRUE(head.is_valid());

  ToolEvent event = at(0.0, 0.0, Qt::LeftButton);
  PickHit hit;
  hit.signal = head;
  hit.road = head_road;
  event.pick = hit;
  const int base = scene.document.undo_stack()->index();
  EXPECT_TRUE(tool.mouse_press(event));
  EXPECT_EQ(scene.selection.primary().signal, head);
  EXPECT_EQ(tool.inspected_junction(), scene.junction)
      << "selecting a head must not drop the junction whose rows are being read";
  EXPECT_EQ(scene.document.undo_stack()->index(), base);
}

} // namespace roadmaker::editor
