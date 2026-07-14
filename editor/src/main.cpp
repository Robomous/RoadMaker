#include "roadmaker/version.hpp"

#include <QApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QSurfaceFormat>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "app/crash_handler.hpp"
#include "app/icons.hpp"
#include "app/log_setup.hpp"
#include "app/main_window.hpp"
#include "app/settings.hpp"
#include "theme/theme.hpp"

namespace {

/// `--screenshot <scene.xodr> <out.png> [--camera top|orbit] [--size WxH]
/// [--textured]` renders the viewport framebuffer (`--textured` opts into the
/// daytime Textured mode; default is the plain Sober look);
/// `--screenshot-ui` captures the whole
/// themed window instead (chrome + docks + viewport — palette mockups and
/// the golden-look capture). Parsed ahead of Qt construction; `valid`
/// false = malformed arguments. `--theme <name>` selects a palette for the
/// session without persisting it (mockups render all three from one build).
struct ScreenshotArgs {
  bool requested = false;
  bool whole_window = false;
  bool valid = true;
  std::filesystem::path scene;
  std::filesystem::path out;
  QString camera = QStringLiteral("orbit");
  QString select;         // OpenDRIVE id to select (captures the selection highlight)
  QString hover;          // OpenDRIVE id to hover (captures the hover highlight)
  QString tool;           // tool id to activate (captures its handle overlay)
  QString raise_dock;     // dock objectName to raise (whole-window captures)
  QString toast;          // toast text to show (captures the toast overlay)
  QString drop_library;   // library item key to drop (captures the created geometry)
  bool show_tour = false; // start the guided-tour overlay (whole-window capture)
  bool textured = false;  // opt into the Textured render mode (default is Sober)
  int width = 1600;
  int height = 1000;
};

ScreenshotArgs parse_screenshot_args(int argc, char** argv) {
  ScreenshotArgs args;
  for (int i = 1; i < argc; ++i) {
    const bool ui_mode = std::strcmp(argv[i], "--screenshot-ui") == 0;
    if (ui_mode || std::strcmp(argv[i], "--screenshot") == 0) {
      args.requested = true;
      args.whole_window = ui_mode;
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
    } else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
      args.select = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
      args.hover = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--tool") == 0 && i + 1 < argc) {
      args.tool = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--raise-dock") == 0 && i + 1 < argc) {
      args.raise_dock = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--toast") == 0 && i + 1 < argc) {
      args.toast = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--drop-library") == 0 && i + 1 < argc) {
      args.drop_library = QString::fromUtf8(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--show-tour") == 0) {
      args.show_tour = true;
    } else if (std::strcmp(argv[i], "--textured") == 0) {
      args.textured = true;
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      // std::from_chars, not sscanf: MSVC deprecates the CRT scanners
      // (C4996 under /WX).
      const std::string_view size(argv[i + 1]);
      const auto parse = [](std::string_view text, int& out) {
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && ptr == text.data() + text.size();
      };
      const std::size_t cross = size.find('x');
      int w = 0, h = 0;
      if (cross != std::string_view::npos && parse(size.substr(0, cross), w) &&
          parse(size.substr(cross + 1), h) && w > 0 && h > 0) {
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

  // "-" captures the launch state (welcome screen) instead of a scene —
  // only meaningful for whole-window shots.
  const bool no_scene = args.scene == "-";
  // load_file reports failures through a modal box — headless runs must
  // never block on one, so reject an unreadable scene up front.
  if (!no_scene && !std::filesystem::exists(args.scene)) {
    std::fprintf(stderr, "screenshot: scene not found: %s\n", args.scene.string().c_str());
    return kScreenshotError;
  }

  roadmaker::editor::MainWindow window(nullptr, /*restore_saved_layout=*/false);
  window.resize(args.width, args.height);
  window.show();
  if (!no_scene) {
    window.load_file(args.scene);
    window.viewport()->set_camera_preset(args.camera);
    window.viewport()->set_hint(QString()); // no tool overlay in captures
  }
  // Sober is the default; --textured opts a capture into the daytime look
  // (used for the golden scene and to showcase the textured mode in CI).
  window.viewport()->set_textured_rendering(args.textured);
  QCoreApplication::processEvents(); // realize the GL widget + first paint
  // Highlights are applied AFTER the first event flush: realizing the widget
  // can deliver a synthetic mouse move that clears a forced hover, so set it
  // last and capture immediately (grabFramebuffer re-renders without pumping
  // the event loop).
  if (!no_scene) {
    window.set_capture_highlights(args.select, args.hover);
    if (!args.tool.isEmpty()) {
      window.activate_tool_for_capture(args.tool);
    }
    if (!args.toast.isEmpty()) {
      window.viewport()->show_toast(args.toast, roadmaker::editor::ToastSeverity::Success);
    }
    if (!args.drop_library.isEmpty()) {
      window.drop_library_item_for_capture(args.drop_library, 0.0, -70.0);
    }
  }
  if (args.show_tour) {
    window.start_tour(); // explicit start bypasses the first-run/seen gate
    QCoreApplication::processEvents();
  }

  if (!args.raise_dock.isEmpty()) {
    window.raise_dock_for_capture(args.raise_dock);
  }

  QImage frame;
  if (args.whole_window) {
    QCoreApplication::processEvents(); // settle dock/toolbar layout first
    frame = window.grab().toImage();   // composites the GL viewport (Qt 6)
  } else {
    frame = window.viewport()->capture_frame();
  }
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
                 "usage: roadmaker-editor --screenshot|--screenshot-ui <scene.xodr> <out.png>"
                 " [--camera top|orbit] [--size WxH] [--theme <name>]"
                 " [--select <odr_id>] [--hover <odr_id>]\n");
    return kScreenshotError;
  }
  QString cli_theme;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--theme") == 0) {
      cli_theme = QString::fromUtf8(argv[i + 1]);
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
  // Window/taskbar icon on every platform (the macOS Dock/Finder icon comes
  // from the bundled .icns, Windows Explorer from the linked .rc/.ico).
  QApplication::setWindowIcon(roadmaker::editor::Icons::app_icon());
  QCoreApplication::setApplicationVersion(QString::fromUtf8(
      roadmaker::version().data(), static_cast<qsizetype>(roadmaker::version().size())));

  // Theme before any window exists: --theme wins for this session only;
  // otherwise the persisted choice; unknown names fall back to the default
  // palette (docs/standards/ui-design.md).
  {
    roadmaker::editor::Settings settings;
    const QString requested = cli_theme.isEmpty() ? settings.theme_name() : cli_theme;
    const roadmaker::editor::Theme* theme = roadmaker::editor::theme::by_name(requested);
    roadmaker::editor::theme::apply(
        app, theme != nullptr ? *theme : roadmaker::editor::theme::default_theme());
  }

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
