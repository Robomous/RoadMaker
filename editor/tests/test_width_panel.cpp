// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// Lane Width panel (p2-s4): width-along-s edits as commands through Document
// (drags = one preview session + one undo entry via set_lane_width_profile),
// insert/remove nodes, and the Shift+double-click lane-section split. Offscreen;
// the panel's public entry points are driven directly — the mouse handlers call
// the same methods.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/editor2d_host.hpp"
#include "panels/width_panel.hpp"

using roadmaker::Lane;
using roadmaker::LaneId;
using roadmaker::LaneProfile;
using roadmaker::Poly3;
using roadmaker::RoadId;
using roadmaker::Waypoint;
using roadmaker::editor::Document;
using roadmaker::editor::SelectionModel;
using roadmaker::editor::WidthEditorPage;
using roadmaker::editor::WidthPanel;

namespace {

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// One two_lane_default road (left +1 driving; right -1 driving, -2 shoulder)
/// with the width panel wired up and lane -1 selected.
struct Rig {
  Document document;
  SelectionModel selection{document};
  WidthPanel panel{document, selection};
  RoadId road;

  explicit Rig(double length = 80.0) {
    if (!document.push_command(roadmaker::edit::create_road(
            {Waypoint{.x = 0.0, .y = 0.0}, Waypoint{.x = length, .y = 0.0}},
            LaneProfile::two_lane_default(),
            "Subject"))) {
      throw std::runtime_error("scene setup failed");
    }
    document.network().for_each_road([&](RoadId id, const roadmaker::Road&) { road = id; });
    select_lane(-1);
  }

  [[nodiscard]] LaneId lane(int odr_id) const {
    const auto& section =
        *document.network().lane_section(document.network().road(road)->sections[0]);
    for (const LaneId lane_id : section.lanes) {
      if (document.network().lane(lane_id)->odr_id == odr_id) {
        return lane_id;
      }
    }
    throw std::runtime_error("lane not found");
  }

  void select_lane(int odr_id) { selection.select({.road = road, .lane = lane(odr_id)}); }

  [[nodiscard]] const Lane& subject() const { return *document.network().lane(lane(-1)); }
};

TEST(WidthPanel, DragNodeIsSingleUndoStep) {
  Rig rig;
  const std::string before = xodr(rig.document);
  const double base_width = rig.subject().widths.front().a;
  const int base_entries = rig.document.undo_stack()->count();

  rig.panel.drag_node(0, 1.0);
  rig.panel.drag_node(0, 1.5); // against the drag base, not cumulative
  EXPECT_TRUE(rig.document.preview_active());
  rig.panel.commit_drag();
  EXPECT_FALSE(rig.document.preview_active());

  EXPECT_EQ(rig.document.undo_stack()->count(), base_entries + 1);
  EXPECT_NEAR(rig.subject().widths.front().a, base_width + 1.5, 1e-9);

  rig.document.undo_stack()->undo();
  EXPECT_EQ(xodr(rig.document), before); // byte-identical restore
}

TEST(WidthPanel, DragMapsToSetLaneWidthProfileNotSetLaneWidth) {
  Rig rig;
  // Insert a second node so the profile is a genuine two-record taper — a shape
  // set_lane_width can never produce (it refuses a multi-record profile). That
  // the drag lands it proves the panel goes through set_lane_width_profile.
  rig.panel.insert_node(40.0);
  ASSERT_EQ(rig.panel.nodes().size(), 2U);

  rig.panel.drag_node(1, 1.0); // raise the s=40 node
  rig.panel.commit_drag();

  const std::vector<Poly3>& widths = rig.subject().widths;
  ASSERT_EQ(widths.size(), 2U);
  const double w0 = widths[0].a;
  const double w1 = widths[1].a;
  // Record 0 is the connecting linear segment to the raised node; record 1 flat.
  EXPECT_NEAR(widths[0].b, (w1 - w0) / 40.0, 1e-9);
  EXPECT_NEAR(widths[1].b, 0.0, 1e-12);
  EXPECT_GT(w1, w0);
}

TEST(WidthPanel, InsertNodeAddsWidthRecord) {
  Rig rig;
  ASSERT_EQ(rig.panel.nodes().size(), 1U);
  const double base_width = rig.subject().widths.front().a;

  rig.panel.insert_node(40.0);
  ASSERT_EQ(rig.panel.nodes().size(), 2U);
  EXPECT_EQ(rig.subject().widths.size(), 2U);
  // Sampled from the (constant) profile, so the new node keeps the width.
  EXPECT_NEAR(rig.panel.nodes().back().width, base_width, 1e-9);
}

TEST(WidthPanel, RemoveNodeDropsRecordAndProtectsSOffsetZero) {
  Rig rig;
  rig.panel.insert_node(30.0);
  rig.panel.insert_node(60.0);
  ASSERT_EQ(rig.panel.nodes().size(), 3U);

  // The sOffset-0 record (index 0) is protected — the kernel requires it.
  rig.panel.remove_node(0);
  EXPECT_EQ(rig.panel.nodes().size(), 3U);

  rig.panel.remove_node(1); // drop the middle node
  ASSERT_EQ(rig.panel.nodes().size(), 2U);
  EXPECT_EQ(rig.subject().widths.size(), 2U);
  // Ascending order preserved.
  EXPECT_LT(rig.panel.nodes()[0].s_offset, rig.panel.nodes()[1].s_offset);
  EXPECT_DOUBLE_EQ(rig.panel.nodes()[0].s_offset, 0.0);
}

TEST(WidthPanel, ShiftDoubleClickSplitsSectionOnceAndIsIdempotentAtABoundary) {
  Rig rig;
  ASSERT_EQ(rig.document.network().road(rig.road)->sections.size(), 1U);
  const int base_entries = rig.document.undo_stack()->count();

  rig.panel.split_at(40.0);
  EXPECT_EQ(rig.document.network().road(rig.road)->sections.size(), 2U);
  EXPECT_EQ(rig.document.undo_stack()->count(), base_entries + 1);

  // The panel re-resolved onto the head section [0, 40). Splitting at its end
  // lands on the existing boundary — idempotent, so no third section appears.
  rig.panel.split_at(40.0);
  EXPECT_EQ(rig.document.network().road(rig.road)->sections.size(), 2U);
}

TEST(WidthPanel, RoundTripThroughXodr) {
  Rig rig;
  // Author a taper via the panel.
  rig.panel.insert_node(40.0);
  rig.panel.drag_node(1, 1.0);
  rig.panel.commit_drag();
  const std::vector<Poly3> authored = rig.subject().widths;
  ASSERT_EQ(authored.size(), 2U);

  const std::string text = xodr(rig.document);
  const auto parsed = roadmaker::parse_xodr(text);
  ASSERT_TRUE(parsed.has_value());
  for (const roadmaker::Diagnostic& diagnostic : parsed->diagnostics) {
    EXPECT_NE(diagnostic.severity, roadmaker::Severity::Error) << diagnostic.message;
  }
  // The reparsed network writes back byte-identically, and lane -1's widths
  // survive record-for-record.
  EXPECT_EQ(*roadmaker::write_xodr(parsed->network), text);

  RoadId reloaded_road;
  parsed->network.for_each_road([&](RoadId id, const roadmaker::Road&) { reloaded_road = id; });
  const auto& section =
      *parsed->network.lane_section(parsed->network.road(reloaded_road)->sections[0]);
  const Lane* reloaded = nullptr;
  for (const LaneId id : section.lanes) {
    if (parsed->network.lane(id)->odr_id == -1) {
      reloaded = parsed->network.lane(id);
    }
  }
  ASSERT_NE(reloaded, nullptr);
  ASSERT_EQ(reloaded->widths.size(), authored.size());
  for (std::size_t i = 0; i < authored.size(); ++i) {
    EXPECT_NEAR(reloaded->widths[i].s, authored[i].s, 1e-9);
    EXPECT_NEAR(reloaded->widths[i].a, authored[i].a, 1e-9);
    EXPECT_NEAR(reloaded->widths[i].b, authored[i].b, 1e-9);
  }
}

TEST(WidthPanel, CancelDragRestoresByteIdentical) {
  Rig rig;
  const std::string before = xodr(rig.document);
  rig.panel.drag_node(0, 2.0);
  EXPECT_TRUE(rig.document.preview_active());
  rig.panel.cancel_drag();
  EXPECT_FALSE(rig.document.preview_active());
  EXPECT_EQ(xodr(rig.document), before);
}

TEST(WidthPanel, RelevantOnlyWithLanePrimary) {
  Rig rig;
  WidthEditorPage page(rig.document, rig.selection);
  EXPECT_EQ(page.title(), QStringLiteral("Lane Width"));

  // A lane is selected (Rig selects -1): relevant, and the panel scopes to it.
  EXPECT_TRUE(page.relevant(rig.selection));
  EXPECT_TRUE(rig.panel.lane().is_valid());

  // Road only, no lane: not relevant, panel inert.
  rig.selection.select({.road = rig.road, .lane = LaneId{}});
  EXPECT_FALSE(page.relevant(rig.selection));
  EXPECT_FALSE(rig.panel.lane().is_valid());

  // The center lane carries no width — nothing to edit there.
  rig.select_lane(0);
  EXPECT_FALSE(rig.panel.lane().is_valid());
}

} // namespace
