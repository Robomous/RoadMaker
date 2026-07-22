// Signal Phase Editor panel (p4-s8, issue #229): headless drives of the timeline
// widget that asserts the panel populates from the kernel cycle, that every edit
// is exactly one undo entry (and undo restores the bytes), that view state
// (scrub/select/next/prev) never touches the undo stack, that a boundary drag is
// ONE preview session committed on release (and cancel reverts byte-identical),
// that Next/Previous wraps and the moving set follows the phase (GW-4 step 10),
// that the playhead's resolved states follow the phase (GW-4 step 5 mechanics),
// that a real Delete QKeyEvent removes the selected phase (GW-4 step 9), and a
// PURE test of build_signal_phase_preview (marker count/colors, moving list,
// gate-link pair count). Never asserts pixels.
//
// Runs under QT_QPA_PLATFORM=offscreen like every other editor test.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/signal_phase_overlay.hpp"
#include "panels/phase_panel.hpp"

namespace roadmaker::editor {
namespace {

using roadmaker::ContactPoint;
using roadmaker::JunctionId;
using roadmaker::LaneProfile;
using roadmaker::RoadEnd;
using roadmaker::RoadId;
using roadmaker::SignalState;
using roadmaker::Waypoint;
using roadmaker::edit::SignalizeTemplate;

std::string xodr(const Document& document) {
  auto text = roadmaker::write_xodr(document.network());
  if (!text) {
    throw std::runtime_error(text.error().message);
  }
  return *text;
}

/// Four roomy arms meeting at the origin, joined into a junction and signalized
/// with the permissive two-phase template (a dynamic cycle to time).
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
    if (!document.push_command(roadmaker::edit::signalize_junction(
            document.network(), junction, {.tmpl = SignalizeTemplate::TwoPhase}))) {
      throw std::runtime_error("signalize failed");
    }
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
};

TEST(PhasePanel, PopulatesFromTheKernelCycle) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();

  EXPECT_EQ(panel.junction(), scene.junction);
  const JunctionPhasePlan& plan = panel.plan();
  EXPECT_FALSE(plan.phases.empty()) << "the two-phase cross has a derived cycle";
  EXPECT_FALSE(plan.controller_odr_ids.empty()) << "one row per member controller";
  EXPECT_EQ(panel.selected_phase(), 0);
  // Every phase's states row-count matches the controller count.
  for (const JunctionPhaseInfo& phase : plan.phases) {
    EXPECT_EQ(phase.states.size(), plan.controller_odr_ids.size());
  }
  // Targeting/populating authors nothing.
  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count);
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(PhasePanel, AddAndDuplicateAreOneUndoEntryEach) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int before = scene.document.undo_stack()->index();
  const std::size_t count = panel.plan().phases.size();

  panel.add_phase();
  EXPECT_EQ(scene.document.undo_stack()->index(), before + 1) << "add is ONE undo step";
  EXPECT_EQ(panel.plan().phases.size(), count + 1);

  panel.duplicate_phase(0);
  EXPECT_EQ(scene.document.undo_stack()->index(), before + 2) << "duplicate is ONE undo step";
  EXPECT_EQ(panel.plan().phases.size(), count + 2);
}

TEST(PhasePanel, DeleteKeyRemovesTheSelectedPhaseAndUndoRestoresTheBytes) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const std::size_t count = panel.plan().phases.size();
  panel.select_phase(1);

  // A real QKeyEvent through the handler (GW-4 step 9).
  QKeyEvent key(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QCoreApplication::sendEvent(&panel, &key);

  EXPECT_EQ(scene.document.undo_stack()->index(), scene.base_count + 1);
  EXPECT_EQ(panel.plan().phases.size(), count - 1);

  scene.document.undo_stack()->undo();
  EXPECT_EQ(xodr(scene.document), scene.base_xodr) << "undo restores the pre-edit bytes";
}

TEST(PhasePanel, ScrubAndSelectionNeverTouchTheUndoStack) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int base = scene.document.undo_stack()->index();

  panel.scrub_to(7.5);
  panel.select_phase(2);
  panel.next_phase();
  panel.prev_phase();

  EXPECT_EQ(scene.document.undo_stack()->index(), base) << "view state is not an edit";
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(PhasePanel, NextPhaseWrapsAndMovingDiffersPerPhase) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int n = static_cast<int>(panel.plan().phases.size());
  ASSERT_GE(n, 2);

  std::vector<std::vector<RoadId>> moving_by_phase;
  panel.select_phase(0);
  for (int i = 0; i < n; ++i) {
    moving_by_phase.push_back(panel.moving_roads());
    panel.next_phase();
  }
  EXPECT_EQ(panel.selected_phase(), 0) << "next from the last phase wraps to the first";

  bool any_differ = false;
  for (std::size_t i = 1; i < moving_by_phase.size(); ++i) {
    if (moving_by_phase[i] != moving_by_phase.front()) {
      any_differ = true;
    }
  }
  EXPECT_TRUE(any_differ) << "the moving set must change across the cycle (GW-4 step 6/10)";
}

TEST(PhasePanel, SignalStatesFollowThePlayhead) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int n = static_cast<int>(panel.plan().phases.size());
  ASSERT_GE(n, 2);

  const auto greens_at = [&](int phase) {
    panel.select_phase(phase);
    int greens = 0;
    for (const PhaseSignalState& head : panel.signal_states_at_playhead()) {
      if (head.state == SignalState::Green) {
        ++greens;
      }
    }
    return greens;
  };

  bool differs = false;
  const int first = greens_at(0);
  for (int i = 1; i < n; ++i) {
    if (greens_at(i) != first) {
      differs = true;
    }
  }
  EXPECT_TRUE(differs) << "the resolved head colors must change per phase (GW-4 step 5)";
}

TEST(PhasePanel, BoundaryDragIsOnePreviewSessionAndOneUndoEntry) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int base = scene.document.undo_stack()->index();

  panel.drag_boundary(0, 4.0);
  EXPECT_TRUE(scene.document.preview_active()) << "the drag runs in a preview session";
  panel.drag_boundary(0, 6.0); // another frame — still one session
  EXPECT_TRUE(scene.document.preview_active());
  panel.commit_drag();

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base + 1)
      << "a drag is ONE command on release, never one per frame";
}

TEST(PhasePanel, CancelledBoundaryDragRevertsByteIdentical) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  const int base = scene.document.undo_stack()->index();

  panel.drag_boundary(0, 5.0);
  ASSERT_TRUE(scene.document.preview_active());
  panel.cancel_drag();

  EXPECT_FALSE(scene.document.preview_active());
  EXPECT_EQ(scene.document.undo_stack()->index(), base) << "a cancelled drag pushes nothing";
  EXPECT_EQ(xodr(scene.document), scene.base_xodr);
}

TEST(PhasePanel, DeletingTheJunctionClearsThePanel) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  ASSERT_TRUE(panel.junction().is_valid());

  ASSERT_TRUE(scene.document.push_command(
      roadmaker::edit::delete_junction(scene.document.network(), scene.junction)));
  EXPECT_FALSE(panel.junction().is_valid()) << "a mesh_changed with the junction gone clears it";
  EXPECT_TRUE(panel.plan().phases.empty());
}

TEST(PhasePanel, LoadedResetsThePanel) {
  Scene scene;
  PhasePanel panel(scene.document, scene.selection);
  scene.select_junction();
  ASSERT_TRUE(panel.junction().is_valid());

  // Round-trip the signalized network through a file, then reload it: loaded()
  // must hard-reset the panel (no junction selected in the fresh document).
  const std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "rm_phase_panel_reset.xodr";
  ASSERT_TRUE(scene.document.save(tmp).has_value());
  ASSERT_TRUE(scene.document.load(tmp).has_value());
  EXPECT_FALSE(panel.junction().is_valid()) << "loaded() clears the target";
  EXPECT_TRUE(panel.plan().phases.empty());
  std::filesystem::remove(tmp);
}

TEST(SignalPhaseOverlay, BuildPreviewResolvesHeadsMovingAndGateLinks) {
  Scene scene;
  const JunctionPhasePlan plan = junction_phases(scene.document.network(), scene.junction);
  ASSERT_FALSE(plan.phases.empty());

  // Pick a phase that actually shows green (phase 0 of the derived cycle is an
  // axis-green phase).
  const JunctionPhaseInfo& phase = plan.phases.front();
  const SignalPhasePreview preview = build_signal_phase_preview(
      scene.document.network(), scene.junction, phase.signal_states, phase.moving);

  // One marker per resolved head that has a world pose; all placed heads resolve.
  EXPECT_EQ(preview.signal_states.size(), phase.signal_states.size());

  // Marker colors match the resolved states (count the greens both ways).
  int plan_greens = 0;
  for (const PhaseSignalState& head : phase.signal_states) {
    if (head.state == SignalState::Green) {
      ++plan_greens;
    }
  }
  int marker_greens = 0;
  for (const SignalPhasePreview::StateMarker& marker : preview.signal_states) {
    if (marker.color == signal_state_color(SignalState::Green)) {
      ++marker_greens;
    }
  }
  EXPECT_EQ(marker_greens, plan_greens);
  EXPECT_GT(plan_greens, 0) << "an axis-green phase lights heads";

  // The moving list passes through unchanged.
  EXPECT_EQ(preview.moving_roads, phase.moving);

  // Gate links are xyz pairs (6 doubles/segment), and a green phase draws some.
  EXPECT_EQ(preview.gate_links.size() % 6, 0U);
  EXPECT_FALSE(preview.gate_links.empty()) << "green heads draw leaders to the movements they gate";
}

} // namespace
} // namespace roadmaker::editor
