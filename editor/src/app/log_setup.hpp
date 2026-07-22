// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Session logging (hardening sprint, #84): one spdlog file sink per editor
// session under AppDataLocation/logs, next to the stderr sink. The file log
// is the command trail crash reports point at — Document logs every executed
// command (with its dirty-set parameters) and every undo/redo through the
// default logger, so a crash report plus its session log reconstructs what
// the user did.

#include <QString>
#include <filesystem>
#include <string>

// This header stays spdlog-free on purpose: spdlog is a PRIVATE dependency of
// the editor library, so its include dirs do not propagate to consumers of this
// header (the app `main`, the tests). The soak level API is therefore a plain
// spdlog level *name* string, resolved to an spdlog level inside the .cpp.

namespace roadmaker::editor::logging {

/// The console log level name the soak runner should use: the `SPDLOG_LEVEL`
/// env var when set/non-empty, else `"critical"` (which silences the expected
/// per-op refusal diagnostics). Pure — for testing the policy. #167.
[[nodiscard]] std::string soak_console_level();

/// Installs a plain stderr default logger at `soak_console_level()` (an
/// unknown/typo name maps to `off`, same as spdlog's env loader). Silences the
/// soak's expected per-op refusals while keeping a genuine crash's output. This
/// MUST live in the editor library: Document and the command layer log through
/// THIS library's spdlog registry, so a set_level/set_default_logger issued from
/// a caller translation unit (the soak `main`) does not reach it. #167.
void set_soak_console_level();

/// "yyyyMMdd-HHmmss-<pid>" — sortable, unique enough per machine, and legal
/// in filenames on all three platforms.
[[nodiscard]] QString make_session_id();

/// QStandardPaths::AppDataLocation / "logs".
[[nodiscard]] std::filesystem::path default_log_dir();

/// The session's log file inside `log_dir` (also how the crash-report dialog
/// finds a *crashed* session's trail from its report's session id).
[[nodiscard]] std::filesystem::path log_file_for(const std::filesystem::path& log_dir,
                                                 const QString& session);

/// Installs the default spdlog logger with a stderr sink plus a file sink at
/// log_file_for(log_dir, session), creating `log_dir` if missing, and prunes
/// all but the newest `keep_files` session logs. Returns the log-file path
/// (empty when the directory could not be created — the stderr sink still
/// works, the editor just runs without a file trail).
std::filesystem::path
init(const std::filesystem::path& log_dir, const QString& session, int keep_files = 10);

} // namespace roadmaker::editor::logging
