// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
