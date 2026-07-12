#include "roadmaker/version.hpp"

#include <QApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QSurfaceFormat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "app/crash_handler.hpp"
#include "app/log_setup.hpp"
#include "app/main_window.hpp"

namespace {

/// `--screenshot <scene.xodr> <out.png> [--camera top|orbit] [--size WxH]`,
/// parsed ahead of Qt construction. `valid` false = malformed arguments.
struct ScreenshotArgs {
  bool requested = false;
  bool valid = true;
  std::filesystem::path scene;
  std::filesystem::path out;
  QString camera = QStringLiteral("orbit");
  int width = 1600;
  int height = 1000;
};

ScreenshotArgs parse_screenshot_args(int argc, char** argv) {
  ScreenshotArgs args;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--screenshot") == 0) {
      args.requested = true;
      if (i + 2 >= argc) {
        args.valid = false;
        return args;
      }
      args.scene = argv[i + 1];
      args.out = argv[i + 2];
      i += 2;
    } else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
      args.camera = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      int w = 0, h = 0;
      if (std::sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        args.width = w;
        args.height = h;
      } else {
        args.valid = false;
      }
      ++i;
    }
  }
  return args;
}

/// Exit codes for screenshot mode. CI treats kScreenshotNoGl as SKIP —
/// headless runners without GL must not fail the job (skip-not-fail rule,
/// docs/contributing/ci.md).
constexpr int kScreenshotOk = 0;
constexpr int kScreenshotError = 1;
constexpr int kScreenshotNoGl = 3;

int run_screenshot(const ScreenshotArgs& args) {
  // Probe GL before building any widget: ViewportWidget treats a failed GL
  // init as fatal (interactive sessions cannot continue), but screenshot
  // mode must degrade to a skip on GL-less runners.
  {
    QOpenGLContext probe;
    if (!probe.create()) {
      std::fprintf(stderr, "screenshot: no OpenGL context available, skipping\n");
      return kScreenshotNoGl;
    }
    QOffscreenSurface surface;
    surface.setFormat(probe.format());
    surface.create();
    if (!surface.isValid() || !probe.makeCurrent(&surface)) {
      std::fprintf(stderr, "screenshot: OpenGL surface unavailable, skipping\n");
      return kScreenshotNoGl;
    }
    probe.doneCurrent();
  }

  // load_file reports failures through a modal box — headless runs must
  // never block on one, so reject an unreadable scene up front.
  if (!std::filesystem::exists(args.scene)) {
    std::fprintf(stderr, "screenshot: scene not found: %s\n", args.scene.string().c_str());
    return kScreenshotError;
  }

  roadmaker::editor::MainWindow window;
  window.resize(args.width, args.height);
  window.show();
  window.load_file(args.scene);
  window.viewport()->set_camera_preset(args.camera);
  window.viewport()->set_hint(QString()); // no tool overlay in captures
  QCoreApplication::processEvents();      // realize the GL widget + first paint

  const QImage frame = window.viewport()->capture_frame();
  if (frame.isNull()) {
    std::fprintf(stderr, "screenshot: framebuffer capture failed\n");
    return kScreenshotError;
  }
  if (!frame.save(QString::fromStdString(args.out.string()))) {
    std::fprintf(stderr, "screenshot: cannot write %s\n", args.out.string().c_str());
    return kScreenshotError;
  }
  std::printf(
      "screenshot: wrote %s (%dx%d)\n", args.out.string().c_str(), frame.width(), frame.height());
  return kScreenshotOk;
}

} // namespace

int main(int argc, char** argv) {
  // --version must work headless (release-bundle smoke tests run it without
  // a display server), so handle it before any Qt construction.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::printf("roadmaker-editor %s\n", std::string(roadmaker::version()).c_str());
      return 0;
    }
  }

  const ScreenshotArgs screenshot = parse_screenshot_args(argc, argv);
  if (screenshot.requested && !screenshot.valid) {
    std::fprintf(stderr,
                 "usage: roadmaker-editor --screenshot <scene.xodr> <out.png>"
                 " [--camera top|orbit] [--size WxH]\n");
    return kScreenshotError;
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
  // macOS creates legacy 2.1 contexts otherwise. 4x MSAA: silhouettes of
  // edge strips and junction boundaries alias badly without it (tee visual
  // finding, follow-up to issue #103).
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  format.setSamples(4);
  QSurfaceFormat::setDefaultFormat(format);

  QApplication app(argc, argv);
  QCoreApplication::setApplicationVersion(QString::fromUtf8(
      roadmaker::version().data(), static_cast<qsizetype>(roadmaker::version().size())));

  if (screenshot.requested) {
    return run_screenshot(screenshot);
  }

  roadmaker::editor::MainWindow window;
  window.show();

  if (argc > 1) {
    window.load_file(argv[1]);
  }

  return QApplication::exec();
}
