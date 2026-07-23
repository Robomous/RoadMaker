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

#include "app/crash_handler.hpp"

#include "roadmaker/version.hpp"

#include <spdlog/spdlog.h>

#include <QDateTime>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtGlobal>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
// Windows has no POSIX signals for hardware faults; the vectored/unhandled
// SEH filter is the equivalent hook, and dbghelp symbolizes the frames.
#include <windows.h>
// windows.h must precede dbghelp.h (dbghelp uses its types unconditionally).
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#if __has_include(<execinfo.h>)
// glibc and macOS ship backtrace(); musl does not — the report then simply
// carries no frames, which is still a report.
#include <execinfo.h>
#define RM_HAVE_EXECINFO 1
#endif
#endif

#ifndef RM_GIT_COMMIT
#define RM_GIT_COMMIT "unknown"
#endif

namespace roadmaker::editor::crash {

namespace {

// Static storage the signal handler may touch: pre-formatted at install()
// (or detail::configure()) so the async-signal-safe path only write()s bytes.
char g_report_path[1024] = {0};
char g_header[2048] = {0};
std::size_t g_header_len = 0;
QString g_session;                // NOLINT — normal-context use only
std::filesystem::path g_log_file; // normal-context use only

constexpr const char* kLogTailMarker = "---- session log tail ----";

/// fopen without MSVC's C4996 deprecation (fopen_s there; plain fopen
/// elsewhere — the CRT "secure" variants are Windows-only).
std::FILE* open_file(const char* path, const char* mode) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
  return std::fopen(path, mode);
#endif
}

void format_header(const std::filesystem::path& report_dir,
                   const std::filesystem::path& log_file,
                   const QString& session) {
  g_session = session;
  g_log_file = log_file;

  const std::string path = (report_dir / ("crash-" + session.toStdString() + ".txt")).string();
  std::snprintf(g_report_path, sizeof(g_report_path), "%s", path.c_str());

  const std::string started = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
  const std::string os = QSysInfo::prettyProductName().toStdString() + " (" +
                         QSysInfo::currentCpuArchitecture().toStdString() + ")";
  const int written = std::snprintf(g_header,
                                    sizeof(g_header),
                                    "RoadMaker crash report\n"
                                    "version: %s (commit %s)\n"
                                    "os: %s\n"
                                    "session: %s (started %s)\n"
                                    "session log: %s\n"
                                    "This report was written locally and was NOT uploaded "
                                    "anywhere.\n\n",
                                    std::string(roadmaker::version()).c_str(),
                                    RM_GIT_COMMIT,
                                    os.c_str(),
                                    session.toStdString().c_str(),
                                    started.c_str(),
                                    log_file.empty() ? "(none)" : log_file.string().c_str());
  g_header_len =
      written > 0 ? std::min(static_cast<std::size_t>(written), sizeof(g_header) - 1) : 0;
}

#if !defined(_WIN32)

// Async-signal-safe helpers — write()/strlen() only.
void write_cstr(int fd, const char* text) {
  // The report is best-effort by definition; a short write mid-crash is not
  // recoverable, so the result is deliberately ignored.
  const auto ignored = ::write(fd, text, std::strlen(text));
  (void)ignored;
}

void write_decimal(int fd, long long value) {
  char buf[32];
  char* cursor = buf + sizeof(buf);
  *--cursor = '\0';
  const bool negative = value < 0;
  unsigned long long magnitude = negative ? 0ULL - static_cast<unsigned long long>(value)
                                          : static_cast<unsigned long long>(value);
  do {
    *--cursor = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0 && cursor > buf + 1);
  if (negative) {
    *--cursor = '-';
  }
  write_cstr(fd, cursor);
}

const char* signal_name(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGFPE:
    return "SIGFPE";
  case SIGILL:
    return "SIGILL";
  case SIGABRT:
    return "SIGABRT";
  default:
    return "signal";
  }
}

volatile sig_atomic_t g_handling = 0;

extern "C" void posix_crash_handler(int sig) {
  // A crash inside the handler must not recurse.
  if (g_handling != 0) {
    _exit(128 + sig);
  }
  g_handling = 1;

  const int fd = ::open(g_report_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  if (fd >= 0) {
    const auto ignored = ::write(fd, g_header, g_header_len);
    (void)ignored;
    write_cstr(fd, "fatal signal: ");
    write_cstr(fd, signal_name(sig));
    write_cstr(fd, " (");
    write_decimal(fd, sig);
    write_cstr(fd, ") at epoch ");
    write_decimal(fd, static_cast<long long>(std::time(nullptr)));
    write_cstr(fd, "\n\nbacktrace:\n");
#if defined(RM_HAVE_EXECINFO)
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fd);
    // macOS only symbolicates exported symbols in-process; the rest print as
    // module + address. `atos -o <binary> <address>` (or addr2line on Linux)
    // recovers the names offline — worth stating inside the report itself.
    write_cstr(fd,
               "\n(frames shown as module + address can be symbolicated offline:\n"
               " macOS: atos -o <roadmaker-editor binary> <address>\n"
               " Linux: addr2line -e <roadmaker-editor binary> <address>)\n");
#else
    write_cstr(fd, "(backtrace unavailable on this platform/libc)\n");
#endif
    ::close(fd);
  }

  // Restore the default disposition and re-raise so the OS crash reporter
  // and exit-code plumbing still see the real crash.
  std::signal(sig, SIG_DFL);
  ::raise(sig);
}

void install_platform_handler() {
  // memset over brace-init: clang-format versions disagree on how to format
  // `struct sigaction action{}` and CI enforces the newer one.
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = posix_crash_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  for (const int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
    sigaction(sig, &action, nullptr);
  }
}

#else // _WIN32

LONG WINAPI seh_crash_filter(EXCEPTION_POINTERS* info) {
  // The filter runs in a broken process; keep to CRT basics (no Qt, no
  // allocation-heavy formatting).
  std::FILE* file = open_file(g_report_path, "w");
  if (file != nullptr) {
    std::fwrite(g_header, 1, g_header_len, file);
    std::fprintf(file,
                 "unhandled SEH exception: code 0x%08lX at epoch %lld\n\nbacktrace:\n",
                 info != nullptr && info->ExceptionRecord != nullptr
                     ? info->ExceptionRecord->ExceptionCode
                     : 0UL,
                 static_cast<long long>(std::time(nullptr)));

    void* frames[62];
    const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    const HANDLE process = GetCurrentProcess();
    const BOOL symbols = SymInitialize(process, nullptr, TRUE);
    for (USHORT i = 0; i < count; ++i) {
      const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
      char buffer[sizeof(SYMBOL_INFO) + 256];
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 255;
      DWORD64 displacement = 0;
      if (symbols == TRUE && SymFromAddr(process, address, &displacement, symbol) == TRUE) {
        std::fprintf(file,
                     "  %2u  %s + 0x%llx\n",
                     i,
                     symbol->Name,
                     static_cast<unsigned long long>(displacement));
      } else {
        std::fprintf(file, "  %2u  0x%llx\n", i, static_cast<unsigned long long>(address));
      }
    }
    if (symbols == TRUE) {
      SymCleanup(process);
    }
    std::fclose(file);
  }
  return EXCEPTION_CONTINUE_SEARCH; // let Windows Error Reporting proceed
}

void install_platform_handler() {
  SetUnhandledExceptionFilter(seh_crash_filter);
}

#endif // _WIN32

void qt_message_handler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  const std::string text = message.toStdString();
  const char* file = context.file != nullptr ? context.file : "";
  switch (type) {
  case QtDebugMsg:
    spdlog::debug("[qt] {}", text);
    break;
  case QtInfoMsg:
    spdlog::info("[qt] {}", text);
    break;
  case QtWarningMsg:
    spdlog::warn("[qt] {} ({})", text, file);
    break;
  case QtCriticalMsg:
    spdlog::error("[qt] {} ({})", text, file);
    break;
  case QtFatalMsg:
    spdlog::critical("[qt fatal] {} ({})", text, file);
    detail::write_report(("Qt fatal: " + text).c_str(), /*with_backtrace=*/true);
    // Qt aborts after a fatal message either way; abort() through our SIGABRT
    // handler would overwrite the richer report just written, so bypass it.
#if !defined(_WIN32)
    std::signal(SIGABRT, SIG_DFL);
#endif
    std::abort();
  }
}

} // namespace

void install(const std::filesystem::path& report_dir,
             const std::filesystem::path& log_file,
             const QString& session) {
  detail::configure(report_dir, log_file, session);
  install_platform_handler();
  qInstallMessageHandler(qt_message_handler);
}

QString current_session() {
  return g_session;
}

std::filesystem::path default_report_dir() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return std::filesystem::path(base.toStdString()) / "crash-reports";
}

std::vector<PendingReport> pending_reports(const std::filesystem::path& report_dir,
                                           const QString& current_session) {
  std::vector<PendingReport> reports;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("crash-") || !name.ends_with(".txt") || name.ends_with(".seen.txt")) {
      continue;
    }
    const QString session =
        QString::fromStdString(name.substr(6, name.size() - 6 - 4)); // crash-<session>.txt
    if (session == current_session) {
      continue;
    }
    reports.push_back(PendingReport{.path = entry.path(), .session = session});
  }
  // Session ids embed a sortable timestamp — newest first without touching
  // file mtimes.
  std::ranges::sort(reports, [](const PendingReport& a, const PendingReport& b) {
    return a.session > b.session;
  });
  return reports;
}

void append_log_tail(const std::filesystem::path& report,
                     const std::filesystem::path& log_file,
                     int max_lines) {
  {
    std::ifstream existing(report);
    std::string line;
    while (std::getline(existing, line)) {
      if (line == kLogTailMarker) {
        return; // already appended on an earlier launch
      }
    }
  }

  std::ofstream out(report, std::ios::app);
  if (!out) {
    return;
  }
  out << '\n' << kLogTailMarker << '\n';

  std::ifstream log(log_file);
  if (!log) {
    out << "(session log " << log_file.string() << " not found)\n";
    return;
  }
  std::deque<std::string> tail;
  std::string line;
  while (std::getline(log, line)) {
    tail.push_back(line);
    if (std::cmp_greater(tail.size(), max_lines)) {
      tail.pop_front();
    }
  }
  for (const std::string& kept : tail) {
    out << kept << '\n';
  }
}

void acknowledge(const PendingReport& report) {
  std::filesystem::path seen = report.path;
  seen.replace_extension(); // crash-<session>
  seen += ".seen.txt";      // crash-<session>.seen.txt
  std::error_code ec;
  std::filesystem::rename(report.path, seen, ec);
  if (ec) {
    spdlog::warn("could not acknowledge crash report {}: {}", report.path.string(), ec.message());
  }
}

namespace detail {

void configure(const std::filesystem::path& report_dir,
               const std::filesystem::path& log_file,
               const QString& session) {
  std::error_code ec;
  std::filesystem::create_directories(report_dir, ec);
  format_header(report_dir, log_file, session);
}

bool write_report(const char* reason, bool with_backtrace) {
  std::FILE* file = open_file(g_report_path, "w");
  if (file == nullptr) {
    return false;
  }
  std::fwrite(g_header, 1, g_header_len, file);
  std::fprintf(file,
               "fatal: %s at epoch %lld\n\nbacktrace:\n",
               reason,
               static_cast<long long>(std::time(nullptr)));
  if (with_backtrace) {
#if defined(RM_HAVE_EXECINFO)
    // backtrace_symbols_fd writes to the raw fd — flush stdio first or the
    // buffered header lands after (and over) the frames at fclose.
    std::fflush(file);
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fileno(file));
#elif defined(_WIN32)
    void* frames[62];
    const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
      std::fprintf(file,
                   "  %2u  0x%llx\n",
                   i,
                   static_cast<unsigned long long>(reinterpret_cast<DWORD64>(frames[i])));
    }
#else
    std::fprintf(file, "(backtrace unavailable on this platform/libc)\n");
#endif
  }
  std::fclose(file);
  return true;
}

} // namespace detail

} // namespace roadmaker::editor::crash
