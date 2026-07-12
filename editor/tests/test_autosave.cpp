// Autosave & crash recovery (M3a issue #53): the write policy and the
// recover-vs-clean decision are driven headless with a fake clock
// (docs/design/m3a/05_editor_and_docs.md §3/§6). The invariant under test
// everywhere: autosave never touches the user's own file.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <array>
#include <fstream>
#include <sstream>

#include "document/autosave.hpp"
#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

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

std::filesystem::path dir_path(const QTemporaryDir& dir) {
  return std::filesystem::path(dir.path().toStdString());
}

TEST(Autosave, TimerPathDebouncesToInterval) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  qint64 now = 0;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [&now] { return now; });

  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  now = AutosaveManager::kIntervalMs - 1;
  autosave.maybe_autosave();
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));

  now = AutosaveManager::kIntervalMs;
  autosave.maybe_autosave();
  EXPECT_TRUE(std::filesystem::exists(autosave.xodr_path()));
  EXPECT_TRUE(std::filesystem::exists(autosave.sidecar_path()));

  // Debounce restarts: an immediate second tick must not rewrite.
  const auto first_write = std::filesystem::last_write_time(autosave.xodr_path());
  ASSERT_TRUE(document.push_command(make_road(20.0, "Second")).has_value());
  autosave.maybe_autosave();
  EXPECT_EQ(std::filesystem::last_write_time(autosave.xodr_path()), first_write);
}

TEST(Autosave, CleanDocumentNeverWrites) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  qint64 now = 0;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [&now] { return now; });

  now = AutosaveManager::kIntervalMs * 10;
  autosave.maybe_autosave();
  ASSERT_TRUE(autosave.autosave_now().has_value()); // explicit call: still a no-op
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));
  EXPECT_FALSE(std::filesystem::exists(autosave.sidecar_path()));
}

TEST(Autosave, EveryNthCommandWritesWithoutTimer) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [] { return qint64{0}; });

  for (int i = 0; i < AutosaveManager::kCommandsPerAutosave - 1; ++i) {
    ASSERT_TRUE(document.push_command(make_road(10.0 * i, "Road")).has_value());
    EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));
  }
  ASSERT_TRUE(document.push_command(make_road(10.0 * AutosaveManager::kCommandsPerAutosave, "Road"))
                  .has_value());
  EXPECT_TRUE(std::filesystem::exists(autosave.xodr_path()));
  EXPECT_TRUE(std::filesystem::exists(autosave.sidecar_path()));
}

TEST(Autosave, PreviewSessionDefersWrites) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  qint64 now = 0;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [&now] { return now; });

  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(document.begin_preview(make_road(50.0, "Dragged")).has_value());
  now = AutosaveManager::kIntervalMs;
  autosave.maybe_autosave();
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path())); // mid-drag state is transient

  document.cancel_preview();
  autosave.maybe_autosave();
  EXPECT_TRUE(std::filesystem::exists(autosave.xodr_path()));
}

TEST(Autosave, CleanSaveAndLoadDeleteTheRecoverySet) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [] { return qint64{0}; });

  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(autosave.autosave_now().has_value());
  ASSERT_TRUE(std::filesystem::exists(autosave.xodr_path()));

  // Clean Save deletes the set (§3 cleanup rule).
  const std::filesystem::path saved = dir_path(dir) / "scene.xodr";
  ASSERT_TRUE(document.save(saved).has_value());
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));
  EXPECT_FALSE(std::filesystem::exists(autosave.sidecar_path()));

  // So does replacing the document via load.
  ASSERT_TRUE(document.push_command(make_road(20.0, "Second")).has_value());
  ASSERT_TRUE(autosave.autosave_now().has_value());
  ASSERT_TRUE(std::filesystem::exists(autosave.xodr_path()));
  ASSERT_TRUE(document.load(saved).has_value());
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));
  EXPECT_FALSE(std::filesystem::exists(autosave.sidecar_path()));
}

TEST(Autosave, NeverTouchesTheUserFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  AutosaveManager autosave(
      document, dir_path(dir), QStringLiteral("session-a"), [] { return qint64{0}; });

  const std::filesystem::path user_file = dir_path(dir) / "scene.xodr";
  ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
  ASSERT_TRUE(document.save(user_file).has_value());
  const std::string user_bytes = file_bytes(user_file);

  ASSERT_TRUE(document.push_command(make_road(40.0, "Second")).has_value());
  ASSERT_TRUE(autosave.autosave_now().has_value());

  EXPECT_EQ(file_bytes(user_file), user_bytes); // byte-identical — untouched
  const auto recovered = roadmaker::load_xodr(autosave.xodr_path());
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->network.road_count(), 2U); // the newer state lives in the copy
}

TEST(Autosave, CrashRecoveryRoundTrip) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path original = dir_path(dir) / "scene.xodr";

  // "Crashed" session: saved once, edited again, autosaved — then gone.
  qint64 now = 1'000;
  {
    Document document;
    AutosaveManager autosave(
        document, dir_path(dir), QStringLiteral("crashed"), [&now] { return now; });
    ASSERT_TRUE(document.push_command(make_road(0.0, "First")).has_value());
    ASSERT_TRUE(document.save(original).has_value());
    ASSERT_TRUE(document.push_command(make_road(40.0, "Second")).has_value());
    ASSERT_TRUE(autosave.autosave_now().has_value());
    ASSERT_TRUE(autosave.autosave_now().has_value()); // token is monotonic
  }

  // The crashed session must not see its own set…
  EXPECT_TRUE(
      AutosaveManager::pending_recoveries(dir_path(dir), QStringLiteral("crashed")).empty());

  // …but the next session does, and the decision says recover.
  const auto sets = AutosaveManager::pending_recoveries(dir_path(dir), QStringLiteral("session-b"));
  ASSERT_EQ(sets.size(), 1U);
  const RecoverySet& set = sets.front();
  EXPECT_EQ(set.session, QStringLiteral("crashed"));
  EXPECT_EQ(set.original_path, QString::fromStdString(original.string()));
  EXPECT_EQ(set.save_token, 2);
  EXPECT_TRUE(AutosaveManager::should_recover(set));

  // Recover: load the copy, re-point at the original, dirty until Save.
  Document document;
  ASSERT_TRUE(document.load(set.xodr).has_value());
  document.mark_recovered(set.original_path);
  EXPECT_TRUE(document.is_dirty());
  EXPECT_EQ(document.file_path(), set.original_path);
  EXPECT_EQ(document.network().road_count(), 2U);

  // The user's file on disk still holds the last clean save.
  const auto on_disk = roadmaker::load_xodr(original);
  ASSERT_TRUE(on_disk.has_value());
  EXPECT_EQ(on_disk->network.road_count(), 1U);

  // Save clears the recovered-dirty state; discard removes the set.
  ASSERT_TRUE(document.save(original).has_value());
  EXPECT_FALSE(document.is_dirty());
  AutosaveManager::discard(set);
  EXPECT_TRUE(
      AutosaveManager::pending_recoveries(dir_path(dir), QStringLiteral("session-b")).empty());
}

TEST(Autosave, SidecarWithoutCopyIsSkipped) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  {
    std::ofstream out(dir_path(dir) / "orphan.json");
    out << R"({"version":1,"originalPath":"","dirty":true,"saveToken":1,"writtenMs":5})";
  }
  EXPECT_TRUE(
      AutosaveManager::pending_recoveries(dir_path(dir), QStringLiteral("session-b")).empty());
}

TEST(Autosave, MissingRecoveryDirYieldsNoCandidates) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  EXPECT_TRUE(AutosaveManager::pending_recoveries(dir_path(dir) / "does-not-exist",
                                                  QStringLiteral("session-b"))
                  .empty());
}

// Hardening sprint §4.6 gap-fill tests.

TEST(Autosave, DisabledManagerWritesNothingAndSweepsItsPair) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path root(dir.path().toStdString());
  Document document;
  qint64 now = 0;
  AutosaveManager autosave(document, root, QStringLiteral("s-disabled"), [&now] { return now; });

  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::create_road(
                      {roadmaker::Waypoint{0.0, 0.0}, roadmaker::Waypoint{50.0, 0.0}},
                      roadmaker::LaneProfile::two_lane_default(),
                      ""))
                  .has_value());
  ASSERT_TRUE(autosave.autosave_now().has_value());
  ASSERT_TRUE(std::filesystem::exists(autosave.xodr_path()));

  autosave.set_enabled(false); // sweeps the pair, stops writing
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));
  now += 10 * AutosaveManager::kIntervalMs;
  autosave.maybe_autosave();
  EXPECT_TRUE(autosave.autosave_now().has_value()); // no-op success
  EXPECT_FALSE(std::filesystem::exists(autosave.xodr_path()));

  autosave.set_enabled(true);
  ASSERT_TRUE(autosave.autosave_now().has_value());
  EXPECT_TRUE(std::filesystem::exists(autosave.xodr_path()));
}

TEST(Autosave, JunctionRegenerationTriggersAPreRegenerationCopy) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path root(dir.path().toStdString());
  Document document;
  qint64 now = 0;
  AutosaveManager autosave(document, root, QStringLiteral("s-regen"), [&now] { return now; });

  const auto road = [&](double ax, double ay, double bx, double by) {
    ASSERT_TRUE(document
                    .push_command(roadmaker::edit::create_road(
                        {roadmaker::Waypoint{ax, ay}, roadmaker::Waypoint{bx, by}},
                        roadmaker::LaneProfile::two_lane_default(),
                        ""))
                    .has_value());
  };
  road(-40.0, 0.0, -6.0, 0.0);
  road(40.0, 0.0, 6.0, 0.0);
  const roadmaker::RoadId west = document.network().find_road("1");
  const roadmaker::RoadId east = document.network().find_road("2");
  const std::array<roadmaker::RoadEnd, 2> ends{
      roadmaker::RoadEnd{.road = west, .contact = roadmaker::ContactPoint::End},
      roadmaker::RoadEnd{.road = east, .contact = roadmaker::ContactPoint::End}};
  ASSERT_TRUE(document.push_command(roadmaker::edit::create_junction(document.network(), ends))
                  .has_value());
  AutosaveManager::discard(RecoverySet{.xodr = autosave.xodr_path(),
                                       .sidecar = autosave.sidecar_path(),
                                       .session = autosave.session()});
  ASSERT_FALSE(std::filesystem::exists(autosave.xodr_path()));

  // Dragging the junction's incoming-road node regenerates the junction —
  // the pre-regeneration copy must exist WITHOUT any timer tick.
  ASSERT_TRUE(document
                  .push_command(roadmaker::edit::move_waypoint(
                      document.network(), west, 0, roadmaker::Waypoint{-42.0, 3.0}))
                  .has_value());
  EXPECT_TRUE(std::filesystem::exists(autosave.xodr_path()));
}

} // namespace
} // namespace roadmaker::editor
