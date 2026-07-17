// CI-cheap packaging check: the staged .qch/.qhc that the build produced open
// as a real Qt Help collection and serve the guide. Running this on all three
// OS every build also exercises the qch generation AND the QSQLITE driver load
// (QHelpEngineCore is SQLite-backed) the shipped viewer depends on.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QHelpEngineCore>
#include <QHelpLink>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "helpc/render.hpp"
#include "helpc/toc.hpp"

namespace roadmaker::editor {
namespace {

// QHelpEngine opens its collection read-write; copy the staged, read-only build
// output into a scratch dir first. The .qch must sit beside the .qhc (the
// collection registers it by a relative path).
QString stage_collection(QTemporaryDir& dir) {
  const std::filesystem::path stage = RM_HELP_STAGE_DIR;
  const QString src_qhc = QString::fromStdString((stage / "roadmaker.qhc").string());
  const QString src_qch = QString::fromStdString((stage / "roadmaker.qch").string());
  const QString dst_qhc = dir.filePath(QStringLiteral("roadmaker.qhc"));
  const QString dst_qch = dir.filePath(QStringLiteral("roadmaker.qch"));
  if (!QFile::copy(src_qhc, dst_qhc) || !QFile::copy(src_qch, dst_qch)) {
    return {};
  }
  return dst_qhc;
}

TEST(HelpCollection, StagedQchRegistersAndServesIndex) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty()) << "could not stage collection from " << RM_HELP_STAGE_DIR;

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();
  EXPECT_TRUE(engine.registeredDocumentations().contains(QStringLiteral("ai.robomous.roadmaker")));

  const QByteArray index =
      engine.fileData(QUrl(QStringLiteral("qthelp://ai.robomous.roadmaker/doc/index.html")));
  EXPECT_FALSE(index.isEmpty()) << "index.html not served from the collection";
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

QByteArray served(QHelpEngineCore& engine, const std::string& rel) {
  return engine.fileData(
      QUrl(QStringLiteral("qthelp://ai.robomous.roadmaker/doc/") + QString::fromStdString(rel)));
}

// Data-driven off the committed guide: every TOC page's rendered HTML must be
// servable from the collection. qhelpgenerator's <file> wildcards do not
// recurse, so tutorials/ pages regress silently without their own pattern
// (#292) — a keyword can exist while its file blob is missing.
TEST(HelpCollection, EveryTocPageIsServed) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty());

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();

  const helpc::Toc toc = helpc::build_toc(std::filesystem::path(RM_DOCS_DIR) / "user-guide");
  ASSERT_FALSE(toc.pages.empty());
  for (const helpc::TocEntry& page : helpc::all_pages(toc)) {
    EXPECT_FALSE(served(engine, page.slug + ".html").isEmpty())
        << page.slug << ".html not served from the collection";
  }
}

// Every image a guide page references must exist in the docs tree AND be
// servable from the collection at the location the page's relative src
// resolves to (tutorials/ pages resolve img/foo.png as tutorials/img/foo.png,
// so the shipped layout must preserve that structure) (#292).
TEST(HelpCollection, EveryReferencedImageIsServed) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty());

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();

  const std::filesystem::path guide = std::filesystem::path(RM_DOCS_DIR) / "user-guide";
  const helpc::Toc toc = helpc::build_toc(guide);
  int image_count = 0;
  for (const helpc::TocEntry& page : helpc::all_pages(toc)) {
    const std::string markdown = read_file(guide / page.rel_path);
    const std::filesystem::path page_dir = std::filesystem::path(page.rel_path).parent_path();
    for (const std::string& target : helpc::extract_image_links(markdown)) {
      if (target.empty() || target.find("://") != std::string::npos) {
        continue; // remote image — nothing to ship
      }
      ++image_count;

      // The committed source the page points at, resolved page-relative.
      const std::filesystem::path source = (guide / page_dir / target).lexically_normal();
      EXPECT_TRUE(std::filesystem::exists(source))
          << page.rel_path << " references missing image " << target;

      // Where the rendered page's <img src> resolves inside the collection.
      std::string copied;
      const std::string rewritten =
          helpc::rewrite_target(target, helpc::RenderOptions{}, /*is_image=*/true, copied);
      const std::string rel = (page_dir / rewritten).lexically_normal().generic_string();
      EXPECT_FALSE(served(engine, rel).isEmpty()) << page.rel_path << " shows " << target << " but "
                                                  << rel << " is not served from the collection";
    }
  }
  EXPECT_GT(image_count, 0) << "coverage test found no image references — extractor broken?";
}

TEST(HelpCollection, EveryTocPageHasAKeyword) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString collection = stage_collection(dir);
  ASSERT_FALSE(collection.isEmpty());

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();

  const helpc::Toc toc = helpc::build_toc(std::filesystem::path(RM_DOCS_DIR) / "user-guide");
  for (const helpc::TocEntry& page : helpc::all_pages(toc)) {
    const QList<QHelpLink> docs = engine.documentsForIdentifier(QString::fromStdString(page.slug));
    EXPECT_FALSE(docs.isEmpty()) << "no keyword id for page slug " << page.slug;
  }
}

} // namespace
} // namespace roadmaker::editor
