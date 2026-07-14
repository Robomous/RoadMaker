#include <spdlog/common.h>

#include <gtest/gtest.h>

#include <QtGlobal>

#include "app/log_setup.hpp"

// The soak runner silences its expected per-op refusal diagnostics by defaulting
// the console threshold to `critical`, while honouring SPDLOG_LEVEL so a local
// debugger can restore the full op trace (#167). These guard the level policy
// (the pure part); the actual suppression is exercised by the soak binary.
namespace roadmaker::editor::logging {
namespace {

TEST(SoakLogging, ConsoleLevelFallsBackWhenEnvUnset) {
  qunsetenv("SPDLOG_LEVEL");
  EXPECT_EQ(console_level_from_env(spdlog::level::critical), spdlog::level::critical);
}

TEST(SoakLogging, ConsoleLevelHonorsEnvOverride) {
  qputenv("SPDLOG_LEVEL", "info");
  EXPECT_EQ(console_level_from_env(spdlog::level::critical), spdlog::level::info);
  qunsetenv("SPDLOG_LEVEL"); // don't leak into sibling tests
}

TEST(SoakLogging, ConsoleLevelMapsUnknownNameToOff) {
  qputenv("SPDLOG_LEVEL", "not-a-level");
  EXPECT_EQ(console_level_from_env(spdlog::level::critical), spdlog::level::off);
  qunsetenv("SPDLOG_LEVEL");
}

} // namespace
} // namespace roadmaker::editor::logging
