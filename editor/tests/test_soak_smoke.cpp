// Soak smoke (hardening sprint §4.3, issue #86): a short, fixed-seed soak in
// the normal suite so every CI run exercises the random-op driver, plus the
// determinism contract the crash-hunt repro workflow depends on.

#include "roadmaker/xodr/writer.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "soak/soak_driver.hpp"

namespace soak = roadmaker::editor::soak;
using roadmaker::editor::Document;
using roadmaker::editor::SelectionModel;

namespace {

soak::SoakOptions options_for(const QTemporaryDir& dir, std::uint32_t seed, int ops) {
  soak::SoakOptions options;
  options.seed = seed;
  options.max_ops = ops;
  options.work_dir = std::filesystem::path(dir.path().toStdString());
  return options;
}

TEST(SoakSmoke, FixedSeedRunsClean) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  Document document;
  SelectionModel selection(document);
  soak::SoakDriver driver(document, selection, options_for(dir, 20260711, 400));
  EXPECT_TRUE(driver.run()) << driver.failure();
  EXPECT_EQ(driver.stats().ops, 400);
  EXPECT_GT(driver.stats().commands, 0);
}

TEST(SoakSmoke, SameSeedSameNetwork) {
  const auto run_once = [](std::uint32_t seed) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    Document document;
    SelectionModel selection(document);
    soak::SoakDriver driver(document, selection, options_for(dir, seed, 150));
    EXPECT_TRUE(driver.run()) << driver.failure();
    const auto xodr = roadmaker::write_xodr(document.network(), "determinism");
    EXPECT_TRUE(xodr.has_value());
    return xodr.value_or("");
  };
  const std::string first = run_once(42);
  const std::string second = run_once(42);
  const std::string different = run_once(43);
  EXPECT_EQ(first, second) << "same seed must replay the same op sequence";
  EXPECT_NE(first, different) << "different seeds should diverge (weak sanity check)";
}

} // namespace
