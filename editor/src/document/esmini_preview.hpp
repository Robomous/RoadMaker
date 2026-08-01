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

// Previewing a scenario in esmini (p8-s5, issue #249, GW-6 step 14).
//
// ★ ESMINI IS LAUNCHED AS A SUBPROCESS AND NOTHING ELSE. It is MPL-2.0, and
// `docs/standards/dependencies.md` admits it as an *external tool*: never
// linked, never bundled, never redistributed. A subprocess launch stays inside
// that entry; linking or shipping it would trigger MPL-2.0's file-level
// copyleft and the entry would not cover it. So there is no esmini header
// anywhere in this tree, no esmini target in any CMakeLists, and this file
// knows only a path and an argument vector.
//
// THE POLICY IS SEPARATED FROM THE LAUNCH, deliberately. `prepare_preview`
// writes the pair and builds the argv; `launch_preview` starts the process.
// Everything worth testing is in the first, which is a pure function of the
// document and a directory — so the offscreen tests exercise exactly the code
// the menu item runs, without ever needing esmini installed.

#include "roadmaker/error.hpp"

#include <QString>
#include <QStringList>
#include <filesystem>

namespace roadmaker::editor {

class Document;

/// Everything needed to start esmini on the current document.
struct EsminiPreview {
  /// The `.xosc` written for this preview, and the `.xodr` beside it. The pair
  /// is stem-matched and lives in one directory, because `<LogicFile>` is a
  /// RELATIVE path resolved against the scenario — the two must travel
  /// together or the simulator resolves nothing.
  std::filesystem::path scenario_path;
  std::filesystem::path network_path;

  /// The argv after the binary. `--headless` is NOT among them: this is the
  /// user asking to watch their scenario, unlike the CI smoke gate.
  QStringList arguments;
};

/// Writes `document`'s network and scenario into `directory` and builds the
/// argument vector for them.
///
/// ★ EXPORTS A COPY RATHER THAN PREVIEWING THE SAVED FILE. A preview must show
/// what is on screen NOW, including edits since the last save, and it must
/// never write into the user's project directory as a side effect of pressing
/// Preview. `directory` is a temporary the caller owns.
///
/// Refuses a document with no scenario worth previewing — an empty `.xosc`
/// loads fine and shows nothing, which looks like a broken preview rather than
/// an empty one — and propagates the writers' own refusals, so a scenario
/// `write_xosc` rejects is reported here rather than as an esmini parse error
/// three seconds later.
[[nodiscard]] Expected<EsminiPreview> prepare_preview(const Document& document,
                                                      const std::filesystem::path& directory);

/// The esmini binary to run, resolved in this order:
///   1. `configured` when it is a non-empty path that exists — the setting the
///      user chose;
///   2. `$ESMINI_PATH`, which is how a CI-like environment points at a fetched
///      binary without touching user settings;
///   3. `esmini` on `PATH`.
/// Empty when none resolves, which the caller turns into "tell the user where
/// to find it" rather than a silent no-op.
[[nodiscard]] QString resolve_esmini(const QString& configured);

/// Starts esmini detached on `preview`. Returns an error when the process
/// could not be started at all; a scenario esmini then rejects is ITS report to
/// make, in its own window.
///
/// Detached on purpose: the editor must not block, and closing esmini must
/// leave the editor untouched (GW-6 step 14).
[[nodiscard]] Expected<void> launch_preview(const QString& binary, const EsminiPreview& preview);

} // namespace roadmaker::editor
