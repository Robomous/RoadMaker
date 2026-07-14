#include "app/log_setup.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <vector>

namespace roadmaker::editor::logging {

QString make_session_id() {
  return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
         QStringLiteral("-") + QString::number(QCoreApplication::applicationPid());
}

std::filesystem::path default_log_dir() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return std::filesystem::path(base.toStdString()) / "logs";
}

std::filesystem::path log_file_for(const std::filesystem::path& log_dir, const QString& session) {
  return log_dir / ("roadmaker-" + session.toStdString() + ".log");
}

namespace {

void prune_old_logs(const std::filesystem::path& log_dir, int keep_files) {
  std::error_code ec;
  std::vector<std::filesystem::path> logs;
  for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
      logs.push_back(entry.path());
    }
  }
  if (std::cmp_less_equal(logs.size(), keep_files)) {
    return;
  }
  // Session ids embed a sortable timestamp, so lexicographic order IS
  // chronological order — no mtime round-trip needed.
  std::ranges::sort(logs);
  const std::size_t excess = logs.size() - static_cast<std::size_t>(keep_files);
  for (std::size_t i = 0; i < excess; ++i) {
    std::filesystem::remove(logs[i], ec);
  }
}

} // namespace

std::string soak_console_level() {
  const char* env = std::getenv("SPDLOG_LEVEL"); // NOLINT(concurrency-mt-unsafe)
  // Only consult the var when the caller actually set something, so an unset
  // var keeps the quiet default rather than an empty name.
  return (env != nullptr && env[0] != '\0') ? std::string(env) : "critical";
}

void set_soak_console_level() {
  auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("roadmaker", std::move(sink));
  // from_str maps an unknown/typo name to `off` — same as spdlog's env loader.
  logger->set_level(spdlog::level::from_str(soak_console_level()));
  spdlog::set_default_logger(std::move(logger));
}

std::filesystem::path
init(const std::filesystem::path& log_dir, const QString& session, int keep_files) {
  std::error_code ec;
  std::filesystem::create_directories(log_dir, ec);

  auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  std::vector<spdlog::sink_ptr> sinks{stderr_sink};

  std::filesystem::path log_file;
  if (!ec) {
    log_file = log_file_for(log_dir, session);
    try {
      sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file.string()));
    } catch (const spdlog::spdlog_ex&) {
      // Unwritable location: keep the stderr sink, run without a file trail.
      log_file.clear();
    }
    prune_old_logs(log_dir, keep_files);
  }

  auto logger = std::make_shared<spdlog::logger>("roadmaker", sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::info);
  logger->flush_on(spdlog::level::info); // a crash must not lose the trail tail
  spdlog::set_default_logger(std::move(logger));
  spdlog::info("session {} started (log: {})",
               session.toStdString(),
               log_file.empty() ? "stderr only" : log_file.string());
  return log_file;
}

} // namespace roadmaker::editor::logging
