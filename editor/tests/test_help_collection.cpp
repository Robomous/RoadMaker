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
