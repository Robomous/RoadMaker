// The context-help map is a single source of truth: F1 routes through
// help::context_page, and every slug it can produce must resolve to a committed
// page under docs/user-guide/ that index.md links (so the help collection and
// the in-app viewer actually serve it). This gate is what stops a new tool or
// panel from shipping without its guide page — exactly #266's acceptance
// criterion. RM_DOCS_DIR is a compile define (editor/tests/CMakeLists.txt).

#include <gtest/gtest.h>

#include <QSet>
#include <QString>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "help/help_registry.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {
namespace {

// The canonical dock objectNames. These MUST match the setObjectName("dock.*")
// calls in editor/src/app/main_window.cpp (build_docks) — a dock renamed there
// without a row in help::dock_table fails this test.
constexpr std::array<const char*, 5> kCanonicalDocks{
    "dock.scene", "dock.library", "dock.properties", "dock.editor2d", "dock.diagnostics"};

// Read a whole file into a QString, or empty when it does not open.
QString read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return {};
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return QString::fromStdString(buffer.str());
}

TEST(HelpRegistry, EveryToolIdHasExactlyOnePage) {
  QSet<int> seen;
  for (const help::ToolPage& row : help::tool_table()) {
    EXPECT_FALSE(seen.contains(static_cast<int>(row.id)))
        << "duplicate help row for ToolId " << static_cast<int>(row.id);
    seen.insert(static_cast<int>(row.id));
    EXPECT_STRNE(row.slug, "") << "every tool row must name a page slug";
  }
  // PropCurve is the last enumerator (tools/tool.hpp); every value at or
  // below it must have a row, so a new ToolId added without one fails here.
  for (int i = 0; i <= static_cast<int>(ToolId::PropCurve); ++i) {
    EXPECT_TRUE(seen.contains(i)) << "ToolId " << i << " has no help page";
    EXPECT_FALSE(help::page_for_tool(static_cast<ToolId>(i)).isEmpty())
        << "page_for_tool(" << i << ") is empty";
  }
}

TEST(HelpRegistry, EveryDockNameHasAPage) {
  for (const char* dock : kCanonicalDocks) {
    EXPECT_FALSE(help::page_for_dock(QString::fromUtf8(dock)).isEmpty())
        << "dock '" << dock << "' has no help page (add a row to help::dock_table)";
  }
  // And every table row is one of the canonical docks — no stale rows.
  QSet<QString> canonical;
  for (const char* dock : kCanonicalDocks) {
    canonical.insert(QString::fromUtf8(dock));
  }
  for (const help::DockPage& row : help::dock_table()) {
    EXPECT_TRUE(canonical.contains(QString::fromUtf8(row.dock)))
        << "dock_table row '" << row.dock << "' is not a canonical dock objectName";
  }
}

// The gate: every slug the map can produce must be a committed page AND linked
// from index.md (so build_toc picks it up and the collection serves it). On a
// miss it prints the exact index.md row to paste — mirroring the shortcut gate.
TEST(HelpRegistry, EveryPageResolvesToACommittedGuidePage) {
  const std::filesystem::path guide = std::filesystem::path(RM_DOCS_DIR) / "user-guide";
  const QString index = read_file(guide / "index.md");
  ASSERT_FALSE(index.isEmpty()) << "missing " << (guide / "index.md").string();

  QSet<QString> slugs;
  for (const help::ToolPage& row : help::tool_table()) {
    slugs.insert(QString::fromUtf8(row.slug));
  }
  for (const help::DockPage& row : help::dock_table()) {
    slugs.insert(QString::fromUtf8(row.slug));
  }

  for (const QString& slug : slugs) {
    const std::filesystem::path page = guide / (slug.toStdString() + ".md");
    EXPECT_TRUE(std::filesystem::exists(page) && std::ifstream(page).is_open())
        << "help page for slug '" << slug.toStdString() << "' is missing: " << page.string()
        << "\nCreate docs/user-guide/" << slug.toStdString() << ".md.";
    const QString link = QStringLiteral("(%1.md)").arg(slug);
    EXPECT_TRUE(index.contains(link))
        << "docs/user-guide/index.md does not link '" << slug.toStdString()
        << "'. Add a row whose link target is " << link.toStdString() << ", e.g.\n| ["
        << slug.toStdString() << "](" << slug.toStdString() << ".md) | … |";
  }
}

TEST(HelpRegistry, ContextPrefersFocusedDockOverActiveTool) {
  // Focus in a dock wins over the armed tool.
  EXPECT_EQ(help::context_page(ToolId::CreateRoad, QStringLiteral("dock.diagnostics")),
            QStringLiteral("diagnostics"));
  // No dock focus -> the active tool's page.
  EXPECT_EQ(help::context_page(ToolId::CreateRoad, QString()), QStringLiteral("create-road"));
  // Neither -> the index.
  EXPECT_EQ(help::context_page(std::nullopt, QString()), QStringLiteral("index"));
  // An unknown dock name is treated as no dock focus.
  EXPECT_EQ(help::context_page(ToolId::LaneCarve, QStringLiteral("dock.nope")),
            QStringLiteral("lane-carve"));
}

} // namespace
} // namespace roadmaker::editor
