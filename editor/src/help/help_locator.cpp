#include "help/help_locator.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <system_error>

#ifndef RM_HELP_VERSION
#define RM_HELP_VERSION "dev"
#endif

namespace roadmaker::editor::help {

namespace {

constexpr const char* kCollectionName = "roadmaker.qhc";
constexpr const char* kDocName = "roadmaker.qch";

std::filesystem::path to_path(const QString& s) {
  return std::filesystem::path(s.toStdString());
}

} // namespace

std::filesystem::path help_dir() {
  const QString app_dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
  // .../RoadMaker.app/Contents/MacOS -> .../Contents/Resources/help
  const QString dir = QDir::cleanPath(app_dir + QStringLiteral("/../Resources/help"));
#else
  const QString dir = QDir::cleanPath(app_dir + QStringLiteral("/help"));
#endif
  return to_path(dir);
}

std::optional<std::filesystem::path> writable_collection() {
  const std::filesystem::path src_dir = help_dir();
  const std::filesystem::path src_qhc = src_dir / kCollectionName;
  const std::filesystem::path src_qch = src_dir / kDocName;

  std::error_code ec;
  if (!std::filesystem::exists(src_qhc, ec) || !std::filesystem::exists(src_qch, ec)) {
    return std::nullopt;
  }

  const QString data_root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (data_root.isEmpty()) {
    return std::nullopt;
  }
  const std::filesystem::path dst_dir = to_path(data_root) / "help" / RM_HELP_VERSION;
  std::filesystem::create_directories(dst_dir, ec);

  const std::filesystem::path dst_qhc = dst_dir / kCollectionName;
  const std::filesystem::path dst_qch = dst_dir / kDocName;
  // Refresh whenever the shipped copy is newer (an upgrade replaces the app but
  // keeps the same versioned dir only if the version string is unchanged).
  const auto copy = std::filesystem::copy_options::overwrite_existing;
  std::filesystem::copy_file(src_qhc, dst_qhc, copy, ec);
  std::filesystem::copy_file(src_qch, dst_qch, copy, ec);
  if (ec) {
    return std::nullopt;
  }
  return dst_qhc;
}

} // namespace roadmaker::editor::help
