#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <fstream>

#include "document/document.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

TEST(Document, LoadEmitsSignalsAndPopulatesNetwork) {
  Document document;
  QSignalSpy loaded_spy(&document, &Document::loaded);
  QSignalSpy mesh_spy(&document, &Document::mesh_changed);
  QSignalSpy diagnostics_spy(&document, &Document::diagnostics_changed);

  const auto result = document.load(kSample);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(loaded_spy.count(), 1);
  EXPECT_EQ(mesh_spy.count(), 1);
  EXPECT_EQ(diagnostics_spy.count(), 1);
  EXPECT_GT(document.network().road_count(), 0U);
  EXPECT_FALSE(document.mesh().roads.empty());
  EXPECT_TRUE(document.has_file());
  EXPECT_EQ(document.undo_stack()->count(), 0);
}

TEST(Document, FailedLoadKeepsPreviousDocument) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());
  const std::size_t roads_before = document.network().road_count();
  const QString path_before = document.file_path();

  QSignalSpy loaded_spy(&document, &Document::loaded);
  QSignalSpy diagnostics_spy(&document, &Document::diagnostics_changed);
  const auto result = document.load("does/not/exist.xodr");

  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(loaded_spy.count(), 0);      // document NOT replaced
  EXPECT_EQ(diagnostics_spy.count(), 1); // error surfaced as diagnostic
  EXPECT_EQ(document.network().road_count(), roads_before);
  EXPECT_EQ(document.file_path(), path_before);
  ASSERT_FALSE(document.diagnostics().empty());
  EXPECT_EQ(document.diagnostics().back().severity, Severity::Error);
}

TEST(Document, MalformedFileReportsErrorNotCrash) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path bad = std::filesystem::path(dir.path().toStdString()) / "bad.xodr";
  {
    std::ofstream out(bad);
    out << "<not-opendrive/>";
  }

  Document document;
  const auto result = document.load(bad);
  // Either channel is acceptable — hard error or diagnostics — but never a
  // silently empty success with no explanation.
  if (result.has_value()) {
    EXPECT_FALSE(document.diagnostics().empty());
  } else {
    EXPECT_FALSE(result.has_value());
  }
}

TEST(Document, ExportGlbWritesValidMagic) {
  Document document;
  ASSERT_TRUE(document.load(kSample).has_value());

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path out = std::filesystem::path(dir.path().toStdString()) / "out.glb";

  const auto result = document.export_glb(out);
  ASSERT_TRUE(result.has_value());
  std::ifstream in(out, std::ios::binary);
  std::array<char, 4> magic{};
  in.read(magic.data(), magic.size());
  EXPECT_TRUE(in.good());
  EXPECT_EQ(std::string_view(magic.data(), 4), "glTF");
}

} // namespace
} // namespace roadmaker::editor
