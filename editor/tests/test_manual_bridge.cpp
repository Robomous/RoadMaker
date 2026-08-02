/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The packaged manual (ADR-0009 / docs-s2): where it installs on each platform,
// how a `rmmanual:` bridge link resolves inside it, what the help compiler emits
// for the bridge section, and the gate that every bridge target is a page that
// exists.

#include <gtest/gtest.h>

#include <QFile>
#include <QHelpEngineCore>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "help/help_browser.hpp"
#include "help/manual_locator.hpp"
#include "helpc/render.hpp"

namespace roadmaker::editor {
namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// ---------------------------------------------------------------- install layout

// One test covers all three platforms because the resolver is a pure function of
// (exe dir, platform) — otherwise two thirds of this would only ever be checked
// by the release job, on a machine nobody is watching.
TEST(ManualLocator, EachPlatformResolvesItsInstallLayout) {
  using help::ManualPlatform;

  EXPECT_EQ(help::manual_dir_for("/Apps/RoadMaker.app/Contents/MacOS", ManualPlatform::kMacOS),
            fs::path("/Apps/RoadMaker.app/Contents/Resources/manual"));
  EXPECT_EQ(help::manual_dir_for("/opt/roadmaker/bin", ManualPlatform::kLinux),
            fs::path("/opt/roadmaker/share/roadmaker/manual"));
  EXPECT_EQ(help::manual_dir_for("C:/Program Files/RoadMaker", ManualPlatform::kWindows),
            fs::path("C:/Program Files/RoadMaker/manual"));
}

TEST(ManualLocator, TheLayoutsAreDistinct) {
  using help::ManualPlatform;
  const fs::path exe = "/somewhere/bin";
  EXPECT_NE(help::manual_dir_for(exe, ManualPlatform::kMacOS),
            help::manual_dir_for(exe, ManualPlatform::kLinux));
  EXPECT_NE(help::manual_dir_for(exe, ManualPlatform::kLinux),
            help::manual_dir_for(exe, ManualPlatform::kWindows));
}

// --------------------------------------------------------------- slug resolution

TEST(ManualLocator, ASlugResolvesToTheFileFormatPage) {
  // The local build uses Astro's `file` format, so a page is `<slug>.html` and
  // NOT `<slug>/index.html` — a directory URL does not open over file://.
  const auto page = help::manual_page_for("/m/manual", "tutorials/getting-around");
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, fs::path("/m/manual/tutorials/getting-around.html"));
}

TEST(ManualLocator, ASlugThatClimbsOutOfTheManualIsRefused) {
  // The slug arrives from a generated document, so it is input, not a constant.
  EXPECT_FALSE(help::manual_page_for("/m/manual", "../../etc/passwd").has_value());
  EXPECT_FALSE(help::manual_page_for("/m/manual", "tutorials/../../secrets").has_value());
  EXPECT_FALSE(help::manual_page_for("/m/manual", "/etc/passwd").has_value());
  EXPECT_FALSE(help::manual_page_for("/m/manual", "").has_value());
  // A backslash means a separator on exactly one platform, so the same slug
  // would resolve to two different files. Refuse rather than normalise.
  EXPECT_FALSE(help::manual_page_for("/m/manual", "tutorials\\..\\..\\x").has_value());
}

TEST(ManualLocator, AnInnocentSlugWithADotIsStillAccepted) {
  const auto page = help::manual_page_for("/m/manual", "reference/v1.2-notes");
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(*page, fs::path("/m/manual/reference/v1.2-notes.html"));
}

// -------------------------------------------------------- the dev-build fallback

TEST(ManualLocator, NoManualShippedWithThisTestBuild) {
  // The premise the fallback test below depends on: bundling is opt-in and the
  // test build never turns it on. If this ever fails, the next test is vacuous.
  EXPECT_FALSE(help::manual_index().has_value())
      << "a manual appeared at " << help::manual_dir().string()
      << ", so the fallback test no longer exercises the fallback";
}

TEST(ManualLocator, TheBridgeReportsFailureWhenNoManualIsBundled) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const fs::path stage = RM_HELP_STAGE_DIR;
  const QString collection = dir.filePath(QStringLiteral("roadmaker.qhc"));
  ASSERT_TRUE(QFile::copy(QString::fromStdString((stage / "roadmaker.qhc").string()), collection));
  ASSERT_TRUE(QFile::copy(QString::fromStdString((stage / "roadmaker.qch").string()),
                          dir.filePath(QStringLiteral("roadmaker.qch"))));

  QHelpEngineCore engine(collection);
  ASSERT_TRUE(engine.setupData()) << engine.error().toStdString();
  help::HelpBrowser browser(engine);

  // False is what selects the "read it online" pointer instead of a dead click.
  // Asserted rather than the message box, because the decision is the behaviour;
  // launching a browser is not something a headless test may do.
  EXPECT_FALSE(browser.open_manual_page(QStringLiteral("tutorials/getting-around")));
}

// ------------------------------------------------------------ the bridge section

TEST(HelpBridge, TheRecognizedSectionYieldsAGuideRelativeSlug) {
  const std::string page = R"(# Lane

Body text with an ordinary link to [Lane Width](lane-width.md).

## Full guide

[Shaping lanes](../tutorials/shaping-lanes.md) — the whole cross-section pass.
)";
  const auto link = helpc::bridge_link(page, "reference/lane-profile.md");
  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(link->text, "Shaping lanes");
  EXPECT_EQ(link->target, "../tutorials/shaping-lanes.md");
  EXPECT_EQ(link->slug, "tutorials/shaping-lanes");
  EXPECT_TRUE(link->anchor.empty());
}

TEST(HelpBridge, APageWithoutTheSectionHasNoBridge) {
  const std::string page = "# Lane\n\n## See also\n\n[Lane Width](lane-width.md)\n";
  EXPECT_FALSE(helpc::bridge_link(page, "reference/lane-profile.md").has_value());
}

TEST(HelpBridge, TheHeadingMustBeAHeadingNotProse) {
  const std::string page = "# Lane\n\nSee the ## Full guide [x](../tutorials/a.md) below\n";
  EXPECT_FALSE(helpc::bridge_link(page, "reference/lane-profile.md").has_value());
}

TEST(HelpBridge, TheSectionEndsAtTheNextHeading) {
  const std::string page = R"(# Lane

## Full guide

Nothing links out of here.

## See also

[Lane Width](lane-width.md)
)";
  EXPECT_FALSE(helpc::bridge_link(page, "reference/lane-profile.md").has_value());
}

TEST(HelpBridge, RenderRewritesOnlyTheBridgeLink) {
  // The same guide is linked twice: once in prose, once as the bridge. Only the
  // second may become a manual link, which is why the renderer rewrites at the
  // parser's offset rather than searching for the text.
  const std::string page = R"(# Lane

As covered in [Shaping lanes](../tutorials/shaping-lanes.md), lanes have widths.

## Full guide

[Shaping lanes](../tutorials/shaping-lanes.md) — the whole cross-section pass.
)";
  helpc::RenderOptions opts;
  opts.title = "Lane";
  opts.page_rel = "reference/lane-profile.md";
  const std::string html = helpc::render_page(page, opts);

  EXPECT_NE(html.find("href=\"rmmanual:tutorials/shaping-lanes\""), std::string::npos) << html;
  // The prose one keeps the pipeline's ordinary treatment (a repo URL).
  EXPECT_NE(html.find("href=\"https://github.com/Robomous/RoadMaker/blob/main/"), std::string::npos)
      << html;
  EXPECT_EQ(html.find("href=\"rmmanual:tutorials/shaping-lanes\"", 0),
            html.rfind("href=\"rmmanual:tutorials/shaping-lanes\""))
      << "exactly one link may be retargeted";
}

TEST(HelpBridge, APageWithNoBridgeRendersExactlyAsBefore) {
  const std::string page = "# Lane\n\n[Lane Width](lane-width.md)\n";
  helpc::RenderOptions opts;
  opts.title = "Lane";
  opts.page_rel = "reference/lane-profile.md";
  const std::string html = helpc::render_page(page, opts);
  EXPECT_EQ(html.find("rmmanual:"), std::string::npos);
  EXPECT_NE(html.find("href=\"lane-width.html\""), std::string::npos) << html;
}

// -------------------------------------------------------------------- the gate

/// Every reference page carrying a bridge section, paired with its target.
std::vector<std::pair<std::string, helpc::BridgeLink>> committed_bridges() {
  std::vector<std::pair<std::string, helpc::BridgeLink>> found;
  const fs::path reference = fs::path(RM_DOCS_DIR) / "user-guide" / "reference";
  for (const auto& entry : fs::directory_iterator(reference)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".md") {
      continue;
    }
    const std::string rel = "reference/" + entry.path().filename().generic_string();
    if (auto link = helpc::bridge_link(read_file(entry.path()), rel)) {
      found.emplace_back(rel, *link);
    }
  }
  return found;
}

TEST(HelpBridge, EveryBridgeTargetIsAPageThatExists) {
  const auto bridges = committed_bridges();
  ASSERT_FALSE(bridges.empty()) << "no reference page carries a '" << helpc::kBridgeHeading
                                << "' section — the convention would be unenforced";

  const fs::path guide = fs::path(RM_DOCS_DIR) / "user-guide";
  for (const auto& [page, link] : bridges) {
    const fs::path target = guide / (link.slug + ".md");
    EXPECT_TRUE(fs::exists(target))
        << page << " bridges to '" << link.target << "' (slug '" << link.slug
        << "'), which is not a committed guide page. Renaming a guide must update "
           "every reference page that bridges to it.";
  }
}

TEST(HelpBridge, EveryBridgeTargetIsInTheGuidesTier) {
  // A bridge points at the site-only tier. One aimed at a reference page would
  // send the reader to the browser for something F1 already had.
  for (const auto& [page, link] : committed_bridges()) {
    EXPECT_TRUE(link.slug.starts_with("tutorials/") || link.slug.starts_with("guides/"))
        << page << " bridges to '" << link.slug << "', which is not in the guides tier";
  }
}

} // namespace
} // namespace roadmaker::editor
