// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Autosave & crash recovery (M3a issue #53,
// docs/design/m3a/05_editor_and_docs.md §3). The recovery copy is a valid
// .xodr written through the same version-explicit writer as Save — never a
// bespoke format — plus a JSON sidecar recording the original document path,
// the dirty state, and a monotonic save token. The write policy (debounce
// interval, every-N-commands) and the recover-vs-clean decision live here so
// a gtest can drive them with a fake clock; the periodic QTimer is a thin
// MainWindow wrapper. Autosave NEVER writes to the user's own file.

#include "roadmaker/error.hpp"

#include <QObject>
#include <QString>
#include <filesystem>
#include <functional>
#include <vector>

namespace roadmaker::editor {

class Document;

/// One session's recovery pair on disk (<session>.xodr + <session>.json).
struct RecoverySet {
  std::filesystem::path xodr;
  std::filesystem::path sidecar;
  QString session;
  QString original_path; ///< empty when the crashed document was never saved
  bool dirty = false;
  qint64 save_token = 0;
  qint64 written_ms = 0; ///< epoch milliseconds of the last autosave
};

class AutosaveManager : public QObject {
  Q_OBJECT

public:
  using Clock = std::function<qint64()>; ///< epoch milliseconds

  static constexpr qint64 kIntervalMs = 30'000;
  static constexpr int kCommandsPerAutosave = 10;

  /// Connects to `document`: committed commands feed the every-N trigger; a
  /// clean Save, a load, or a reset deletes the recovery set (the copy would
  /// describe a document that is no longer open). `clock` defaults to
  /// QDateTime::currentMSecsSinceEpoch; tests inject a fake.
  AutosaveManager(Document& document,
                  std::filesystem::path recovery_dir,
                  QString session_id,
                  Clock clock = {},
                  QObject* parent = nullptr);

  /// Timer tick: writes when the document is dirty, no preview drag is in
  /// flight, and at least kIntervalMs elapsed since the last write.
  void maybe_autosave();

  /// Master switch (default on; the editor persists it as a setting). A
  /// disabled manager writes nothing and sweeps its own recovery pair so a
  /// stale copy never outlives the choice.
  void set_enabled(bool enabled);

  [[nodiscard]] bool enabled() const { return enabled_; }

  /// Writes the recovery pair now (still skipped when the document is clean
  /// or a preview session is active — mid-drag state is transient).
  [[nodiscard]] Expected<void> autosave_now();

  /// Deletes this session's recovery pair (clean save / load / close).
  void clear_recovery();

  [[nodiscard]] const QString& session() const { return session_; }

  [[nodiscard]] std::filesystem::path xodr_path() const;
  [[nodiscard]] std::filesystem::path sidecar_path() const;

  /// Recovery sets left behind by other sessions (i.e. crashes), newest
  /// first. Sidecars without a readable recovery .xodr are skipped.
  [[nodiscard]] static std::vector<RecoverySet>
  pending_recoveries(const std::filesystem::path& recovery_dir, const QString& current_session);

  /// The recover-vs-clean decision: only a set autosaved while dirty is
  /// newer than its session's last clean save — a clean save deletes the
  /// set, so a surviving clean one is stale and safe to sweep.
  [[nodiscard]] static bool should_recover(const RecoverySet& set);

  /// Removes the pair from disk (user chose Discard, or recovery succeeded).
  static void discard(const RecoverySet& set);

  /// QStandardPaths::AppDataLocation / "recovery".
  [[nodiscard]] static std::filesystem::path default_recovery_dir();

private:
  void on_command_committed();

  bool enabled_ = true;
  Document& document_;
  std::filesystem::path dir_;
  QString session_;
  Clock clock_;
  qint64 last_autosave_ms_ = 0;
  qint64 save_token_ = 0;
  int commands_since_autosave_ = 0;
};

} // namespace roadmaker::editor
