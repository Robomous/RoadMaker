// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Crash-capture infrastructure (hardening sprint, #84). On a fatal signal,
// an unhandled Windows SEH exception, or a Qt fatal message, a best-effort
// crash report is written locally: timestamp, version + commit, OS, a native
// stack trace, and a pointer to the session log (whose last lines the
// next-launch dialog appends into the report — the command trail is half the
// value). NO telemetry, NO network: reports never leave the machine, and
// the dialog says so.
//
// The POSIX signal path is async-signal-safe: everything formattable is
// pre-formatted into static storage at install() time, and the handler only
// calls open()/write()/close()/backtrace_symbols_fd(), then re-raises the
// default disposition so the OS crash reporter still runs.

#include <QString>
#include <filesystem>
#include <vector>

namespace roadmaker::editor::crash {

/// Installs the Qt message handler (Qt messages route into spdlog; a fatal
/// one writes a report) and the platform crash handler. Creates `report_dir`
/// if missing. `log_file` is referenced in the report; `session` names this
/// run's report file (crash-<session>.txt).
void install(const std::filesystem::path& report_dir,
             const std::filesystem::path& log_file,
             const QString& session);

/// The session id install() was called with (empty before install()).
[[nodiscard]] QString current_session();

/// QStandardPaths::AppDataLocation / "crash-reports".
[[nodiscard]] std::filesystem::path default_report_dir();

/// A crash report left behind by an earlier session.
struct PendingReport {
  std::filesystem::path path;
  QString session;
};

/// Unacknowledged reports from other sessions, newest first (session ids
/// embed a sortable timestamp). The current session's own report name and
/// *.seen.txt files are skipped.
[[nodiscard]] std::vector<PendingReport> pending_reports(const std::filesystem::path& report_dir,
                                                         const QString& current_session);

/// Appends the last `max_lines` lines of `log_file` to `report`. Idempotent
/// (a report already carrying a tail is left untouched); a missing log file
/// appends a note instead so the reader isn't left guessing.
void append_log_tail(const std::filesystem::path& report,
                     const std::filesystem::path& log_file,
                     int max_lines = 200);

/// Renames the report to crash-<session>.seen.txt so the next-launch dialog
/// offers each report exactly once. The file itself stays on disk for the
/// user to attach to an issue.
void acknowledge(const PendingReport& report);

namespace detail {

/// Points the report machinery at `report_dir` WITHOUT installing any
/// handler — what tests use to exercise write_report() against a temp dir.
void configure(const std::filesystem::path& report_dir,
               const std::filesystem::path& log_file,
               const QString& session);

/// The normal-context report writer (Qt fatal path and tests; the signal
/// handler runs an async-signal-safe twin producing the same layout).
/// Returns false when the report file could not be opened.
bool write_report(const char* reason, bool with_backtrace);

} // namespace detail

} // namespace roadmaker::editor::crash
