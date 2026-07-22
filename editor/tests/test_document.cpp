// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>

#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

std::unique_ptr<edit::Command> make_road(double y, const char* name) {
  return edit::create_road({Waypoint{.x = 0.0, .y = y}, Waypoint{.x = 100.0, .y = y}},
                           LaneProfile::two_lane_default(),
                           name);
}

std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return std::move(out).str();
}

TEST(Document, LoadEmitsSignalsAndPopulatesNetwork) {
  Document document;
  QSignalSpy loaded_spy(&document, &Document::loaded);
  QSignalSpy mesh_spy(&document, &Document::mesh_changed);
  QSignalSpy diagnostics_spy(&document, &Document::diagnostics_changed);

  const auto result = document.load(kSample);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(loaded_spy.count(), 1);
  EXPECT_EQ(mesh_spy.count(), 1);
  EXPECT_EQ(diagnostics_spy.count(), 1);
  EXPECT_GT(document.network().road_count(), 0U);
  EXPECT_FALSE(document.mesh().roads.empty());
  EXPECT_TRUE(document.has_file());
  EXPECT_EQ(document.undo_stack()->count(), 0);
}

TEST(Document, FailedLoadKeepsPreviousDocument) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  const std::size_t roads_before = document.network().road_count();
  const QString path_before = document.file_path();

  QSignalSpy loaded_spy(&document, &Document::loaded);
  QSignalSpy diagnostics_spy(&document, &Document::diagnostics_changed);
  const auto result = document.load("does/not/exist.xodr");

  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(loaded_spy.count(), 0);      // document NOT replaced
  EXPECT_EQ(diagnostics_spy.count(), 1); // error surfaced as diagnostic
  EXPECT_EQ(document.network().road_count(), roads_before);
  EXPECT_EQ(document.file_path(), path_before);
  ASSERT_FALSE(document.diagnostics().empty());
  EXPECT_EQ(document.diagnostics().back().severity, Severity::Error);
}

TEST(Document, MalformedFileReportsErrorNotCrash) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path bad = std::filesystem::path(dir.path().toStdString()) / "bad.xodr";
  {
    std::ofstream out(bad);
    out << "<not-opendrive/>";
  }

  Document document;
  const auto result = document.load(bad);
  // Either channel is acceptable — hard error or diagnostics — but never a
  // silently empty success with no explanation.
  if (result.has_value()) {
    EXPECT_FALSE(document.diagnostics().empty());
  } else {
    EXPECT_FALSE(result.has_value());
  }
}

TEST(Document, ExportGlbWritesValidMagic) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path out = std::filesystem::path(dir.path().toStdString()) / "out.glb";

  const auto result = document.export_glb(out);
  ASSERT_TRUE(result.has_value());
  std::ifstream in(out, std::ios::binary);
  std::array<char, 4> magic{};
  in.read(magic.data(), magic.size());
  EXPECT_TRUE(in.good());
  EXPECT_EQ(std::string_view(magic.data(), 4), "glTF");
}

// The Phase 1 gate (issue #12, docs/design/m2/02_editing_tools.md §8):
// new → author → save → reload → byte-equal.
TEST(Document, NewAuthorSaveReloadIsByteEqual) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "scene.xodr";

  Document document;
  document.reset();
  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(document.push_command(make_road(40.0, "Second")).has_value());

  QSignalSpy saved_spy(&document, &Document::saved);
  ASSERT_TRUE(document.save(path).has_value());
  EXPECT_EQ(saved_spy.count(), 1);
  EXPECT_EQ(document.file_path(), QString::fromStdString(path.string()));
  EXPECT_TRUE(document.diagnostics().empty()); // authored network is valid

  Document reloaded;
  ASSERT_TRUE(reloaded.load(path).has_value());
  EXPECT_EQ(reloaded.network().road_count(), 2U);
  const auto rewritten = roadmaker::write_xodr(reloaded.network(), "scene");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, file_bytes(path));
}

TEST(Document, DirtyFlagLifecycle) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "scene.xodr";

  Document document;
  EXPECT_FALSE(document.is_dirty()); // fresh document is clean

  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  EXPECT_TRUE(document.is_dirty());

  ASSERT_TRUE(document.save(path).has_value());
  EXPECT_FALSE(document.is_dirty()); // setClean() on successful save

  ASSERT_TRUE(document.push_command(make_road(40.0, "Second")).has_value());
  EXPECT_TRUE(document.is_dirty());

  document.undo_stack()->undo();
  EXPECT_FALSE(document.is_dirty()); // undo back to the saved state

  document.undo_stack()->redo();
  EXPECT_TRUE(document.is_dirty());
}

TEST(Document, ResetClearsDocumentAndEmitsLoadSignals) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  ASSERT_TRUE(document.push_command(make_road(500.0, "Extra")).has_value());
  ASSERT_TRUE(document.is_dirty());

  QSignalSpy loaded_spy(&document, &Document::loaded);
  QSignalSpy mesh_spy(&document, &Document::mesh_changed);
  QSignalSpy diagnostics_spy(&document, &Document::diagnostics_changed);
  document.reset();

  EXPECT_EQ(loaded_spy.count(), 1);
  EXPECT_EQ(mesh_spy.count(), 1);
  EXPECT_EQ(diagnostics_spy.count(), 1);
  EXPECT_EQ(document.network().road_count(), 0U);
  EXPECT_FALSE(document.has_file());
  EXPECT_TRUE(document.diagnostics().empty());
  EXPECT_EQ(document.undo_stack()->count(), 0); // New is not undoable
  EXPECT_FALSE(document.is_dirty());
}

TEST(Document, SaveEmptyDocumentIsValidAndReloads) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "empty.xodr";

  Document document;
  ASSERT_TRUE(document.save(path).has_value()); // header-only document (§8)

  Document reloaded;
  ASSERT_TRUE(reloaded.load(path).has_value());
  EXPECT_EQ(reloaded.network().road_count(), 0U);
  EXPECT_TRUE(reloaded.diagnostics().empty());
}

// #215 Scope C: after_kernel_mutation reconciles enclosed-area ground surfaces
// off the existing dirty fields — derive on a topology change, remesh on a
// bounding-road move — and it runs on redo AND undo, keeping undo exact.
std::unique_ptr<edit::Command> seg(double x0, double y0, double x1, double y1, const char* name) {
  return edit::create_road({Waypoint{.x = x0, .y = y0}, Waypoint{.x = x1, .y = y1}},
                           LaneProfile::two_lane_default(),
                           name);
}

TEST(Document, DerivesGroundSurfaceForAnEnclosedLoop) {
  Document document;
  ASSERT_TRUE(document.push_command(seg(0.0, 0.0, 20.0, 0.0, "a")).has_value());
  ASSERT_TRUE(document.push_command(seg(20.0, 0.0, 20.0, 20.0, "b")).has_value());
  ASSERT_TRUE(document.push_command(seg(20.0, 20.0, 0.0, 20.0, "c")).has_value());
  EXPECT_EQ(document.network().surface_count(), 0U) << "three roads enclose nothing";

  // Closing the loop derives one surface, and build_network_mesh's surface
  // channel is populated so it renders.
  ASSERT_TRUE(document.push_command(seg(0.0, 20.0, 0.0, 0.0, "d")).has_value());
  EXPECT_EQ(document.network().surface_count(), 1U);
  EXPECT_EQ(document.mesh().surfaces.size(), 1U);

  // Undo opens the loop → the surface vanishes (undo stays exact); redo brings
  // it back. Both route through after_kernel_mutation, which re-derives.
  document.undo_stack()->undo();
  EXPECT_EQ(document.network().surface_count(), 0U);
  EXPECT_TRUE(document.mesh().surfaces.empty());

  document.undo_stack()->redo();
  EXPECT_EQ(document.network().surface_count(), 1U);
  EXPECT_EQ(document.mesh().surfaces.size(), 1U);
}

double surface_max_z(const Document& document) {
  double max_z = -1e9;
  for (const SurfaceMesh& sm : document.mesh().surfaces) {
    for (std::size_t i = 2; i < sm.mesh.positions.size(); i += 3) {
      max_z = std::max(max_z, sm.mesh.positions[i]);
    }
  }
  return max_z;
}

TEST(Document, RaisingABoundingRoadRemeshesTheSurface) {
  Document document;
  ASSERT_TRUE(document.push_command(seg(0.0, 0.0, 20.0, 0.0, "a")).has_value());
  ASSERT_TRUE(document.push_command(seg(20.0, 0.0, 20.0, 20.0, "b")).has_value());
  ASSERT_TRUE(document.push_command(seg(20.0, 20.0, 0.0, 20.0, "c")).has_value());
  ASSERT_TRUE(document.push_command(seg(0.0, 20.0, 0.0, 0.0, "d")).has_value());
  ASSERT_EQ(document.mesh().surfaces.size(), 1U);
  EXPECT_NEAR(surface_max_z(document), 0.0, 1e-6) << "flat loop, flat surface";

  // A pure geometry edit (elevation — no topology change) keeps the plan-view
  // ring closed, so the surface SET is unchanged, but a bounding road moved:
  // after_kernel_mutation must remesh the surfaces touching it. The height
  // field follows the raised road, so the surface's peak z climbs.
  RoadId first;
  document.network().for_each_road([&](RoadId id, const Road&) {
    if (!first.is_valid()) {
      first = id;
    }
  });
  ASSERT_TRUE(document.push_command(edit::set_node_elevation(document.network(), first, 0, 5.0))
                  .has_value());
  EXPECT_EQ(document.mesh().surfaces.size(), 1U);
  EXPECT_GT(surface_max_z(document), 0.5) << "the surface remeshed to follow the raised road";
}

} // namespace
} // namespace roadmaker::editor
