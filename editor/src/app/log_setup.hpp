#pragma once

// Session logging (hardening sprint, #84): one spdlog file sink per editor
// session under AppDataLocation/logs, next to the stderr sink. The file log
// is the command trail crash reports point at — Document logs every executed
// command (with its dirty-set parameters) and every undo/redo through the
// default logger, so a crash report plus its session log reconstructs what
// the user did.

#include <spdlog/common.h> // spdlog::level::level_enum

#include <QString>
#include <filesystem>

namespace roadmaker::editor::logging {

/// Resolves a console log threshold from the `SPDLOG_LEVEL` env var, returning
/// `fallback` when it is unset/empty (an unknown/typo name maps to `off` via
/// spdlog, same as spdlog's own env loader). Pure — for testing the policy.
[[nodiscard]] spdlog::level::level_enum console_level_from_env(spdlog::level::level_enum fallback);

/// Installs a plain stderr default logger at `console_level_from_env(fallback)`.
/// Used by the soak runner to silence the expected per-op refusal diagnostics
/// (they log at warn/error) while keeping a genuine crash's output. This MUST
/// live in the editor library: Document and the command layer log through THIS
/// library's spdlog registry, so a set_level/set_default_logger issued from a
/// caller translation unit (the soak `main`) does not reach it. #167.
void set_console_level_from_env(spdlog::level::level_enum fallback);

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
