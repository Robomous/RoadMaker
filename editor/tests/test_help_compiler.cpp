// The (Qt-free) help compiler: TOC extraction from index.md, page rendering
// with link rewrites, and .qhp emission. Temp-dir fixtures — no QApplication.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "helpc/qhp.hpp"
#include "helpc/render.hpp"
#include "helpc/toc.hpp"

namespace roadmaker::helpc {
namespace {

class HelpCompiler : public ::testing::Test {
protected:
  std::filesystem::path guide;

  void SetUp() override {
    static std::atomic<int> counter{0};
    guide = std::filesystem::temp_directory_path() /
            ("rm_helpc_test_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(guide);
    std::filesystem::create_directories(guide);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(guide, ec);
  }

  void write(const std::string& rel, const std::string& content) {
    const std::filesystem::path path = guide / rel;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << content;
  }
};

TEST_F(HelpCompiler, TocFollowsIndexLinkOrder) {
  write("index.md", "# Guide\n\n[Beta](b.md) then [Alpha](a.md) then [Gamma](c.md)\n");
  write("a.md", "# Alpha\n");
  write("b.md", "# Beta\n");
  write("c.md", "# Gamma\n");

  const Toc toc = build_toc(guide);
  ASSERT_EQ(toc.pages.size(), 3U);
  EXPECT_EQ(toc.pages[0].slug, "b");
  EXPECT_EQ(toc.pages[1].slug, "a");
  EXPECT_EQ(toc.pages[2].slug, "c");
  EXPECT_EQ(toc.index.slug, "index");
  EXPECT_EQ(toc.index.title, "Guide");
}

TEST_F(HelpCompiler, TocDropsLinksOutsideTheGuide) {
  write("index.md",
        "# Guide\n\n"
        "[In](inside.md) "
        "[Up](../getting-started/building.md) "
        "[Ext](https://example.com/x.md) "
        "[Gone](missing.md) "
        "[Img](img/pic.png)\n");
  write("inside.md", "# Inside\n");

  const Toc toc = build_toc(guide);
  ASSERT_EQ(toc.pages.size(), 1U) << "only the existing in-guide .md link survives";
  EXPECT_EQ(toc.pages[0].slug, "inside");
}

TEST_F(HelpCompiler, PageTitleComesFromH1) {
  write("index.md", "# Guide\n\n[P](page.md)\n");
  write("page.md", "Intro line\n\n# The Real Title\n\n## A subsection\n");

  const Toc toc = build_toc(guide);
  ASSERT_EQ(toc.pages.size(), 1U);
  EXPECT_EQ(toc.pages[0].title, "The Real Title");
}

TEST_F(HelpCompiler, MdLinksRewriteToHtmlAndExternalLinksToGitHub) {
  RenderOptions opts;
  opts.title = "T";
  const std::string html = render_page(
      "See [the page](create-road.md) and the [build guide](../getting-started/building.md).\n",
      opts);

  EXPECT_NE(html.find("href=\"create-road.html\""), std::string::npos) << html;
  EXPECT_NE(html.find("href=\"https://github.com/Robomous/RoadMaker/blob/main/docs/getting-started/"
                      "building.md\""),
            std::string::npos)
      << html;
}

TEST_F(HelpCompiler, QhpEscapesXmlSpecials) {
  Toc toc;
  toc.index.slug = "index";
  toc.index.title = "Guide & <Home>";
  TocEntry page;
  page.slug = "pg";
  page.title = R"(Quotes "and" <angles> & amps)";
  toc.pages.push_back(page);

  const std::string qhp = build_qhp(toc, {});
  EXPECT_NE(qhp.find("Guide &amp; &lt;Home&gt;"), std::string::npos) << qhp;
  EXPECT_NE(qhp.find("&quot;and&quot; &lt;angles&gt; &amp; amps"), std::string::npos) << qhp;
  // No unescaped metacharacter leaks into an attribute value.
  EXPECT_EQ(qhp.find("<Home>"), std::string::npos);
}

} // namespace
} // namespace roadmaker::helpc
