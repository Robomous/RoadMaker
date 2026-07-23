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

#pragma once

// Where the shipped Qt Help collection lives, and how to obtain a WRITABLE copy
// of it. QHelpEngine opens its collection read-write (it caches into the .qhc),
// but the collection ships inside a read-only, code-signed app bundle — so it
// must be copied into a per-user, versioned data directory first.

#include <filesystem>
#include <optional>

namespace roadmaker::editor::help {

/// The virtual-help identifiers baked into the .qch by the help compiler.
inline constexpr const char* kNamespace = "ai.robomous.roadmaker";
inline constexpr const char* kFolder = "doc";

/// The GitHub user-guide URL used as the fallback when the collection is
/// unavailable.
inline constexpr const char* kGithubUserGuideUrl =
    "https://github.com/Robomous/RoadMaker/tree/main/docs/user-guide";

/// Directory the built collection ships in, next to the executable:
/// `Contents/Resources/help` on macOS, `help` beside the binary elsewhere.
[[nodiscard]] std::filesystem::path help_dir();

/// Copy the shipped `.qhc` + `.qch` into a versioned, writable data directory
/// (QStandardPaths::AppDataLocation) and return the copied collection file.
/// Returns nullopt when the shipped collection is missing.
[[nodiscard]] std::optional<std::filesystem::path> writable_collection();

} // namespace roadmaker::editor::help
