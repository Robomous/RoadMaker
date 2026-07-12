// Standalone soak runner (hardening sprint §4.3, issue #86):
//
//   roadmaker_soak [--seed N] [--ops N] [--minutes M] [--dir PATH]
//
// Runs seeded-random editing operations against a real Document until a
// bound is hit or an invariant fails; exit code 1 on failure with the seed
// in the message (same seed = same sequence). CI runs it under ASan for
// ~10 minutes; longer local runs are the sprint's crash hunt.

#include <QApplication>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "soak/soak_driver.hpp"

int main(int argc, char** argv) {
  // Default to offscreen so plain local invocations work without a display;
  // an explicit QT_QPA_PLATFORM (e.g. xcb under xvfb) wins.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);

  roadmaker::editor::soak::SoakOptions options;
  options.max_ops = 5000;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0) {
      options.seed = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    } else if (std::strcmp(argv[i], "--ops") == 0) {
      options.max_ops = std::atoi(argv[i + 1]);
    } else if (std::strcmp(argv[i], "--minutes") == 0) {
      options.max_seconds = std::strtod(argv[i + 1], nullptr) * 60.0;
      options.max_ops = 0; // time-bounded runs go until the clock says stop
    } else if (std::strcmp(argv[i], "--dir") == 0) {
      options.work_dir = std::filesystem::path(argv[i + 1]);
    }
  }

  QTemporaryDir temp_dir;
  if (options.work_dir.empty()) {
    if (!temp_dir.isValid()) {
      std::fprintf(stderr, "soak: no writable work dir\n");
      return 2;
    }
    options.work_dir = std::filesystem::path(temp_dir.path().toStdString());
  }

  std::printf("soak: seed=%u ops=%d minutes=%.1f\n",
              options.seed,
              options.max_ops,
              options.max_seconds / 60.0);
  std::fflush(stdout);

  roadmaker::editor::Document document;
  roadmaker::editor::SelectionModel selection(document);
  roadmaker::editor::soak::SoakDriver driver(document, selection, options);
  const bool passed = driver.run();

  const auto& stats = driver.stats();
  std::printf("soak: %d ops (%d commands, %d previews, %d undo / %d redo, %d saves, "
              "%d rejected)\n",
              stats.ops,
              stats.commands,
              stats.previews,
              stats.undos,
              stats.redos,
              stats.saves,
              stats.rejected);
  if (!passed) {
    std::fprintf(stderr, "%s\n", driver.failure().c_str());
    std::fprintf(
        stderr, "reproduce with: roadmaker_soak --seed %u --ops %d\n", options.seed, stats.ops + 1);
    return 1;
  }
  std::printf("soak: PASS (seed %u)\n", options.seed);
  return 0;
}
