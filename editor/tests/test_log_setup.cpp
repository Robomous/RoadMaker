#include <gtest/gtest.h>

#include <QtGlobal>

#include "app/log_setup.hpp"

// The soak runner silences its expected per-op refusal diagnostics by defaulting
// the console level to "critical", while honouring SPDLOG_LEVEL so a local
// debugger can restore the full op trace (#167). These guard the level-name
// policy (the pure part); the actual suppression is exercised by the soak
// binary. The header stays spdlog-free (spdlog is a private editor-lib dep), so
// the policy is expressed as a level *name* string.
namespace roadmaker::editor::logging {
namespace {

TEST(SoakLogging, LevelDefaultsToCriticalWhenEnvUnset) {
  qunsetenv("SPDLOG_LEVEL");
  EXPECT_EQ(soak_console_level(), "critical");
}

TEST(SoakLogging, LevelHonorsEnvOverride) {
  qputenv("SPDLOG_LEVEL", "info");
  EXPECT_EQ(soak_console_level(), "info");
  qunsetenv("SPDLOG_LEVEL"); // don't leak into sibling tests
}

TEST(SoakLogging, LevelIgnoresEmptyEnv) {
  qputenv("SPDLOG_LEVEL", "");
  EXPECT_EQ(soak_console_level(), "critical");
  qunsetenv("SPDLOG_LEVEL");
}

} // namespace
} // namespace roadmaker::editor::logging
