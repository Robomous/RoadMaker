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

#include "document/autosave.hpp"

#include "roadmaker/xodr/writer.hpp"

#include <spdlog/spdlog.h>

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUndoStack>
#include <algorithm>
#include <system_error>
#include <utility>

#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

constexpr int kSidecarVersion = 1;

std::filesystem::path
member_path(const std::filesystem::path& dir, const QString& session, const char* extension) {
  return dir / (session.toStdString() + extension);
}

} // namespace

AutosaveManager::AutosaveManager(Document& document,
                                 std::filesystem::path recovery_dir,
                                 QString session_id,
                                 Clock clock,
                                 QObject* parent)
    : QObject(parent), document_(document), dir_(std::move(recovery_dir)),
      session_(std::move(session_id)),
      clock_(clock ? std::move(clock) : [] { return QDateTime::currentMSecsSinceEpoch(); }),
      last_autosave_ms_(clock_()) {
  connect(document_.undo_stack(),
          &QUndoStack::indexChanged,
          this,
          &AutosaveManager::on_command_committed);
  connect(&document_, &Document::saved, this, &AutosaveManager::clear_recovery);
  connect(&document_, &Document::loaded, this, &AutosaveManager::clear_recovery);
  // A recovery copy of the pre-regeneration state: junction regeneration is
  // the hairiest lifetime zone dogfooding found (hardening §4.6).
  connect(&document_, &Document::about_to_regenerate, this, [this] {
    if (auto written = autosave_now(); !written.has_value()) {
      spdlog::warn("pre-regeneration autosave failed: {}", written.error().message);
    }
  });
}

void AutosaveManager::set_enabled(bool enabled) {
  enabled_ = enabled;
  if (!enabled_) {
    clear_recovery();
  }
}

void AutosaveManager::on_command_committed() {
  if (!document_.is_dirty()) {
    // Undo back to the clean state, or a stack clear on load/reset —
    // nothing worth protecting.
    commands_since_autosave_ = 0;
    return;
  }
  if (++commands_since_autosave_ < kCommandsPerAutosave) {
    return;
  }
  if (auto written = autosave_now(); !written.has_value()) {
    spdlog::warn("autosave failed: {}", written.error().message);
  }
}

void AutosaveManager::maybe_autosave() {
  if (!enabled_) {
    return;
  }
  if (!document_.is_dirty() || document_.preview_active()) {
    return;
  }
  if (clock_() - last_autosave_ms_ < kIntervalMs) {
    return;
  }
  if (auto written = autosave_now(); !written.has_value()) {
    spdlog::warn("autosave failed: {}", written.error().message);
  }
}

Expected<void> AutosaveManager::autosave_now() {
  if (!enabled_) {
    return {};
  }
  if (!document_.is_dirty() || document_.preview_active()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    return make_error(
        ErrorCode::IoFailure, "cannot create recovery directory: " + ec.message(), dir_.string());
  }

  // Write-then-rename so a crash mid-write never leaves a truncated copy
  // where the startup scanner will find it.
  const std::filesystem::path target = xodr_path();
  std::filesystem::path tmp = target;
  tmp += ".tmp";
  const QString original = document_.file_path();
  const std::string name = original.isEmpty()
                               ? std::string("recovery")
                               : std::filesystem::path(original.toStdString()).stem().string();
  if (auto written = roadmaker::save_xodr(document_.network(), tmp, name); !written.has_value()) {
    return written;
  }
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    return make_error(
        ErrorCode::IoFailure, "cannot move recovery file: " + ec.message(), target.string());
  }

  const qint64 now = clock_();
  const QJsonObject sidecar{
      {QStringLiteral("version"), kSidecarVersion},
      {QStringLiteral("originalPath"), original},
      {QStringLiteral("dirty"), true},
      {QStringLiteral("saveToken"), ++save_token_},
      {QStringLiteral("writtenMs"), now},
  };
  QSaveFile file(QString::fromStdString(sidecar_path().string()));
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(QJsonDocument(sidecar).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
    return make_error(
        ErrorCode::IoFailure, "cannot write recovery sidecar", sidecar_path().string());
  }
  last_autosave_ms_ = now;
  commands_since_autosave_ = 0;
  return {};
}

void AutosaveManager::clear_recovery() {
  std::error_code ec;
  std::filesystem::remove(xodr_path(), ec);
  std::filesystem::remove(sidecar_path(), ec);
  commands_since_autosave_ = 0;
}

std::filesystem::path AutosaveManager::xodr_path() const {
  return member_path(dir_, session_, ".xodr");
}

std::filesystem::path AutosaveManager::sidecar_path() const {
  return member_path(dir_, session_, ".json");
}

std::vector<RecoverySet>
AutosaveManager::pending_recoveries(const std::filesystem::path& recovery_dir,
                                    const QString& current_session) {
  std::vector<RecoverySet> sets;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(recovery_dir, ec)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    const QString session = QString::fromStdString(entry.path().stem().string());
    if (session == current_session) {
      continue;
    }
    QFile file(QString::fromStdString(entry.path().string()));
    if (!file.open(QIODevice::ReadOnly)) {
      continue;
    }
    const QJsonDocument sidecar = QJsonDocument::fromJson(file.readAll());
    if (!sidecar.isObject()) {
      continue;
    }
    const QJsonObject object = sidecar.object();
    RecoverySet set{
        .xodr = member_path(recovery_dir, session, ".xodr"),
        .sidecar = entry.path(),
        .session = session,
        .original_path = object.value(QStringLiteral("originalPath")).toString(),
        .dirty = object.value(QStringLiteral("dirty")).toBool(false),
        .save_token = object.value(QStringLiteral("saveToken")).toVariant().toLongLong(),
        .written_ms = object.value(QStringLiteral("writtenMs")).toVariant().toLongLong(),
    };
    std::error_code exists_ec;
    if (!std::filesystem::exists(set.xodr, exists_ec)) {
      continue; // sidecar without its recovery copy — nothing to offer
    }
    sets.push_back(std::move(set));
  }
  std::ranges::sort(
      sets, [](const RecoverySet& a, const RecoverySet& b) { return a.written_ms > b.written_ms; });
  return sets;
}

bool AutosaveManager::should_recover(const RecoverySet& set) {
  return set.dirty;
}

void AutosaveManager::discard(const RecoverySet& set) {
  std::error_code ec;
  std::filesystem::remove(set.xodr, ec);
  std::filesystem::remove(set.sidecar, ec);
}

std::filesystem::path AutosaveManager::default_recovery_dir() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return std::filesystem::path(base.toStdString()) / "recovery";
}

} // namespace roadmaker::editor
