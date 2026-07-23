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
