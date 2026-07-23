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

// Crash-capture infrastructure (#84): report writing, pending-report
// detection/acknowledgement, and the log-tail append. The signal/SEH
// handlers themselves are exercised through detail::write_report — the
// normal-context twin producing the same report layout — because raising a
// real SIGSEGV inside a gtest process would take the runner down with it.

#include "roadmaker/version.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/crash_handler.hpp"

namespace crash = roadmaker::editor::crash;

namespace {

std::string read_all(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_lines(const std::filesystem::path& path, int count, const std::string& prefix) {
  std::ofstream out(path);
  for (int i = 0; i < count; ++i) {
    out << prefix << i << '\n';
  }
}

class CrashHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.isValid());
    root_ = std::filesystem::path(dir_.path().toStdString());
    log_file_ = root_ / "session.log";
    crash::detail::configure(root_, log_file_, QStringLiteral("20260711-120000-42"));
  }

  QTemporaryDir dir_;
  std::filesystem::path root_;
  std::filesystem::path log_file_;
};

TEST_F(CrashHandlerTest, WriteReportProducesHeaderAndReason) {
  ASSERT_TRUE(crash::detail::write_report("test crash", /*with_backtrace=*/false));

  const auto report = root_ / "crash-20260711-120000-42.txt";
  ASSERT_TRUE(std::filesystem::exists(report));
  const std::string text = read_all(report);
  EXPECT_NE(text.find("RoadMaker crash report"), std::string::npos);
  EXPECT_NE(text.find(std::string(roadmaker::version())), std::string::npos);
  EXPECT_NE(text.find("session: 20260711-120000-42"), std::string::npos);
  EXPECT_NE(text.find("fatal: test crash"), std::string::npos);
  EXPECT_NE(text.find("NOT uploaded"), std::string::npos);
  EXPECT_NE(text.find(log_file_.string()), std::string::npos);
}

TEST_F(CrashHandlerTest, WriteReportWithBacktraceEmitsFrames) {
  ASSERT_TRUE(crash::detail::write_report("test crash", /*with_backtrace=*/true));
  const std::string text = read_all(root_ / "crash-20260711-120000-42.txt");
  ASSERT_NE(text.find("backtrace:"), std::string::npos);
  // Everything after the marker is platform-dependent; a non-empty section
  // is the portable assertion (musl builds legitimately print a notice).
  EXPECT_GT(text.size(), text.find("backtrace:") + std::string("backtrace:\n").size());
}

TEST_F(CrashHandlerTest, PendingReportsSkipsOwnSessionAndSeen) {
  ASSERT_TRUE(crash::detail::write_report("own session", false));
  write_lines(root_ / "crash-20260710-090000-7.txt", 3, "old");
  write_lines(root_ / "crash-20260711-080000-9.txt", 3, "new");
  write_lines(root_ / "crash-20260709-010101-1.seen.txt", 3, "seen");
  write_lines(root_ / "unrelated.txt", 1, "x");

  const auto pending = crash::pending_reports(root_, QStringLiteral("20260711-120000-42"));
  ASSERT_EQ(pending.size(), 2U);
  // Newest first — session ids embed a sortable timestamp.
  EXPECT_EQ(pending[0].session, QStringLiteral("20260711-080000-9"));
  EXPECT_EQ(pending[1].session, QStringLiteral("20260710-090000-7"));
}

TEST_F(CrashHandlerTest, AcknowledgeRenamesToSeenExactlyOnce) {
  write_lines(root_ / "crash-20260710-090000-7.txt", 3, "old");
  auto pending = crash::pending_reports(root_, QStringLiteral("20260711-120000-42"));
  ASSERT_EQ(pending.size(), 1U);

  crash::acknowledge(pending.front());
  EXPECT_TRUE(std::filesystem::exists(root_ / "crash-20260710-090000-7.seen.txt"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "crash-20260710-090000-7.txt"));
  EXPECT_TRUE(crash::pending_reports(root_, QStringLiteral("20260711-120000-42")).empty());
}

TEST_F(CrashHandlerTest, AppendLogTailKeepsLastLinesAndIsIdempotent) {
  ASSERT_TRUE(crash::detail::write_report("tail test", false));
  const auto report = root_ / "crash-20260711-120000-42.txt";
  write_lines(log_file_, 300, "line");

  crash::append_log_tail(report, log_file_, 200);
  std::string text = read_all(report);
  EXPECT_EQ(text.find("line99\n"), std::string::npos);  // trimmed
  EXPECT_NE(text.find("line100\n"), std::string::npos); // first kept line
  EXPECT_NE(text.find("line299\n"), std::string::npos); // last kept line

  // A second launch must not duplicate the tail.
  crash::append_log_tail(report, log_file_, 200);
  const std::string again = read_all(report);
  EXPECT_EQ(again, text);
}

TEST_F(CrashHandlerTest, AppendLogTailNotesMissingLog) {
  ASSERT_TRUE(crash::detail::write_report("no log", false));
  const auto report = root_ / "crash-20260711-120000-42.txt";

  crash::append_log_tail(report, root_ / "does-not-exist.log", 200);
  EXPECT_NE(read_all(report).find("not found"), std::string::npos);
}

TEST_F(CrashHandlerTest, PendingReportsOnMissingDirIsEmpty) {
  EXPECT_TRUE(crash::pending_reports(root_ / "nope", QStringLiteral("s")).empty());
}

} // namespace
