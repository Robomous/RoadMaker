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

#include "document/esmini_preview.hpp"

#include "roadmaker/osc/edit.hpp"
#include "roadmaker/osc/writer.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringLiteral>
#include <string>

#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

/// The stem the exported pair takes. Fixed rather than derived from the scene's
/// name: it lands in a throwaway directory, and a fixed name keeps the argv
/// stable enough to assert on.
constexpr const char* kStem = "preview";

} // namespace

Expected<EsminiPreview> prepare_preview(const Document& document,
                                        const std::filesystem::path& directory) {
  if (!document.has_scenario()) {
    return make_error(ErrorCode::InvalidArgument,
                      "this scene has no scenario to preview — place an actor first",
                      directory.string());
  }

  EsminiPreview preview;
  preview.network_path = directory / (std::string(kStem) + ".xodr");
  preview.scenario_path = directory / (std::string(kStem) + ".xosc");

  if (auto written = roadmaker::save_xodr(document.network(), preview.network_path, kStem);
      !written) {
    return tl::unexpected<Error>(written.error());
  }

  // ★ THE <LogicFile> IS REWRITTEN TO THE EXPORTED NETWORK'S NAME, and it must
  // be. The document's own link points at the scene's `.xodr` — which is
  // somewhere else entirely, or nowhere at all when the scene has never been
  // saved — so previewing without this rewrite either resolves a STALE network
  // (the last save, not what is on screen) or nothing.
  osc::Scenario scenario = document.scenario();
  osc::edit::ScenarioStack stack;
  if (auto pointed = stack.push(
          scenario, osc::edit::set_logic_file(scenario, preview.network_path.filename().string()));
      !pointed) {
    return tl::unexpected<Error>(pointed.error());
  }

  if (auto written = osc::save_xosc(scenario, preview.scenario_path); !written) {
    return tl::unexpected<Error>(written.error());
  }

  preview.arguments = QStringList{
      QStringLiteral("--osc"),
      QString::fromStdString(preview.scenario_path.string()),
      // A fixed timestep keeps a preview reproducible between machines, and
      // `--window` asks for a window explicitly: this is the user watching,
      // not the CI gate, which uses `--headless`.
      QStringLiteral("--fixed_timestep"),
      QStringLiteral("0.05"),
      QStringLiteral("--window"),
      QStringLiteral("60"),
      QStringLiteral("60"),
      QStringLiteral("1200"),
      QStringLiteral("800"),
  };
  return preview;
}

QString resolve_esmini(const QString& configured) {
  if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) {
    return configured;
  }
  // The environment before PATH: a CI-like setup points at a fetched binary
  // without writing anything into the user's settings.
  const QString from_env = qEnvironmentVariable("ESMINI_PATH");
  if (!from_env.isEmpty() && QFileInfo(from_env).isExecutable()) {
    return from_env;
  }
  return QStandardPaths::findExecutable(QStringLiteral("esmini"));
}

Expected<void> launch_preview(const QString& binary, const EsminiPreview& preview) {
  if (binary.isEmpty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "no esmini binary was found — set its path in Preferences, or put "
                      "'esmini' on PATH",
                      "esmini");
  }
  // DETACHED: the editor must not block, and closing esmini must leave the
  // editor untouched (GW-6 step 14). The working directory is the export
  // directory so the scenario's relative <LogicFile> resolves.
  const QString working = QString::fromStdString(preview.scenario_path.parent_path().string());
  if (!QProcess::startDetached(binary, preview.arguments, working)) {
    return make_error(ErrorCode::IoFailure, "esmini could not be started", binary.toStdString());
  }
  return {};
}

} // namespace roadmaker::editor
