#include "roadmaker/version.hpp"

#include <QApplication>
#include <QSurfaceFormat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "app/crash_handler.hpp"
#include "app/log_setup.hpp"
#include "app/main_window.hpp"

int main(int argc, char** argv) {
  // --version must work headless (release-bundle smoke tests run it without
  // a display server), so handle it before any Qt construction.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::printf("roadmaker-editor %s\n", std::string(roadmaker::version()).c_str());
      return 0;
    }
  }

  // Org/app names first: QStandardPaths derives the log and crash-report
  // locations from them, and crash capture must cover the WHOLE session —
  // including QApplication construction (the name setters are static, so
  // they work before the instance exists).
  QCoreApplication::setOrganizationName(QStringLiteral("Robomous"));
  QCoreApplication::setApplicationName(QStringLiteral("RoadMaker"));
  const QString session = roadmaker::editor::logging::make_session_id();
  const std::filesystem::path log_file =
      roadmaker::editor::logging::init(roadmaker::editor::logging::default_log_dir(), session);
  roadmaker::editor::crash::install(
      roadmaker::editor::crash::default_report_dir(), log_file, session);

  // 3.3 core profile must be the app-wide default BEFORE QApplication —
  // macOS creates legacy 2.1 contexts otherwise.
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication app(argc, argv);
  QCoreApplication::setApplicationVersion(QString::fromUtf8(
      roadmaker::version().data(), static_cast<qsizetype>(roadmaker::version().size())));

  roadmaker::editor::MainWindow window;
  window.show();

  if (argc > 1) {
    window.load_file(argv[1]);
  }

  return QApplication::exec();
}
