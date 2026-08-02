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

// Where the packaged HTML manual lives (ADR-0009 / docs-s2), and what to do when
// it does not — which is the normal case for a developer build, since bundling
// it is opt-in (`ROADMAKER_BUNDLE_MANUAL`, default OFF) and needs Node.
//
// The layout question is deliberately a PURE function of an executable directory
// and a platform, so all three platforms' answers are checked by one headless
// test on whichever platform happens to be running it. `help_locator.hpp` is the
// same shape for the Qt Help collection.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace roadmaker::editor::help {

/// Install layouts the manual ships in. Named rather than `#ifdef`-ed so a test
/// can ask about a platform it is not running on.
enum class ManualPlatform : std::uint8_t {
  kMacOS,   ///< RoadMaker.app/Contents/Resources/manual
  kLinux,   ///< share/roadmaker/manual, with the executable in bin/
  kWindows, ///< manual/ beside the executable
};

/// The platform this build targets.
[[nodiscard]] ManualPlatform this_platform();

/// Directory the manual is installed in, for an executable in `exe_dir`.
/// Pure: no filesystem access, no Qt, no globals.
[[nodiscard]] std::filesystem::path manual_dir_for(const std::filesystem::path& exe_dir,
                                                   ManualPlatform platform);

/// `manual_dir_for` against the running executable's directory.
[[nodiscard]] std::filesystem::path manual_dir();

/// The manual's entry page if the manual actually shipped, else nullopt. This is
/// the whole dev-build fallback decision: nullopt means "point at the web docs".
[[nodiscard]] std::optional<std::filesystem::path> manual_index();

/// The file a `rmmanual:<slug>` link resolves to inside `manual_root`.
///
/// The local build uses Astro's `file` format, so `tutorials/getting-around`
/// is `tutorials/getting-around.html` — not a directory with an index. Pure, and
/// it REFUSES a slug that climbs out of the manual (`..`), because the slug
/// arrives from a generated document rather than from this code.
[[nodiscard]] std::optional<std::filesystem::path>
manual_page_for(const std::filesystem::path& manual_root, const std::string& slug);

} // namespace roadmaker::editor::help
