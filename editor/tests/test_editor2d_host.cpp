// The 2D Editor pane's host (P1/GW-2 step 7). The host's whole job is tab
// management: pages keep their own selection subscriptions and their own
// commands, which is why test_profile_panel.cpp needed no changes to keep
// passing once the panel was hosted.

#include <gtest/gtest.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QTabWidget>
#include <memory>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "panels/editor2d_host.hpp"

namespace roadmaker::editor {
namespace {

const std::filesystem::path kSample = std::filesystem::path(RM_SAMPLES_DIR) / "t_junction.xodr";

struct Harness {
  Document document;
  SelectionModel selection{document};
};

std::vector<RoadId> all_roads(const Document& document) {
  std::vector<RoadId> roads;
  document.network().for_each_road([&](RoadId id, const Road&) { roads.push_back(id); });
  return roads;
}

/// A page that is relevant only when told to be — lets the raise logic be
/// tested without dragging real editors in.
class FakePage : public Editor2DPage {
public:
  FakePage(QString title, bool relevant) : title_(std::move(title)), relevant_(relevant) {}

  [[nodiscard]] QString title() const override { return title_; }

  [[nodiscard]] QWidget* widget() override { return &widget_; }

  [[nodiscard]] bool relevant(const SelectionModel&) const override { return relevant_; }

  void set_relevant(bool relevant) { relevant_ = relevant; }

private:
  QString title_;
  bool relevant_;
  QWidget widget_;
};

TEST(Editor2DHost, RegistersPagesAsTabsInOrder) {
  Harness h;
  Editor2DHost host(h.selection);
  EXPECT_EQ(host.page_count(), 0);

  host.register_page(std::make_unique<FakePage>(QStringLiteral("First"), false));
  host.register_page(std::make_unique<FakePage>(QStringLiteral("Second"), false));

  EXPECT_EQ(host.page_count(), 2);
  EXPECT_EQ(host.current_title(), QStringLiteral("First"));
}

TEST(Editor2DHost, RaisesTheFirstRelevantPage) {
  Harness h;
  Editor2DHost host(h.selection);
  host.register_page(std::make_unique<FakePage>(QStringLiteral("Irrelevant"), false));
  host.register_page(std::make_unique<FakePage>(QStringLiteral("Relevant"), true));

  host.raise_relevant_page();
  EXPECT_EQ(host.current_title(), QStringLiteral("Relevant"));
}

// A user who picked a tab keeps it: yanking it away mid-edit because the
// selection moved would be worse than showing a stale one.
TEST(Editor2DHost, KeepsACurrentTabThatIsStillRelevant) {
  Harness h;
  Editor2DHost host(h.selection);
  host.register_page(std::make_unique<FakePage>(QStringLiteral("A"), true));
  host.register_page(std::make_unique<FakePage>(QStringLiteral("B"), true));

  auto* tabs = host.findChild<QTabWidget*>(QStringLiteral("editor2d_tabs"));
  ASSERT_NE(tabs, nullptr);
  tabs->setCurrentIndex(1);
  ASSERT_EQ(host.current_title(), QStringLiteral("B"));

  host.raise_relevant_page();
  EXPECT_EQ(host.current_title(), QStringLiteral("B")) << "both are relevant — don't switch";
}

TEST(Editor2DHost, LeavesTheTabAloneWhenNothingIsRelevant) {
  Harness h;
  Editor2DHost host(h.selection);
  host.register_page(std::make_unique<FakePage>(QStringLiteral("A"), false));
  host.register_page(std::make_unique<FakePage>(QStringLiteral("B"), false));

  auto* tabs = host.findChild<QTabWidget*>(QStringLiteral("editor2d_tabs"));
  tabs->setCurrentIndex(1);
  host.raise_relevant_page();
  EXPECT_EQ(host.current_title(), QStringLiteral("B")) << "no arbitrary pick";
}

// The migration: the profile editor works hosted exactly as it did standing
// alone, and is relevant precisely when a road is selected.
TEST(ProfileEditorPage, HostsTheProfilePanelAndFollowsRoadSelection) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());

  auto page = std::make_unique<ProfileEditorPage>(h.document, h.selection);
  ProfileEditorPage* raw = page.get();
  EXPECT_EQ(raw->title(), QStringLiteral("Vertical Profile"));
  ASSERT_NE(raw->panel(), nullptr);
  EXPECT_FALSE(raw->relevant(h.selection)) << "nothing selected yet";

  const RoadId road = all_roads(h.document).front();
  h.selection.select({.road = road});
  EXPECT_TRUE(raw->relevant(h.selection));
  // The panel kept its OWN subscription — the host never forwards selection.
  EXPECT_EQ(raw->panel()->road(), road);

  Editor2DHost host(h.selection);
  host.register_page(std::move(page));
  EXPECT_EQ(host.page_count(), 1);
  EXPECT_EQ(host.current_title(), QStringLiteral("Vertical Profile"));
}

TEST(Editor2DHost, SelectionChangeRaisesTheRelevantPage) {
  Harness h;
  ASSERT_TRUE(h.document.load(kSample).has_value());
  Editor2DHost host(h.selection);
  host.register_page(std::make_unique<FakePage>(QStringLiteral("Never"), false));
  host.register_page(std::make_unique<ProfileEditorPage>(h.document, h.selection));
  ASSERT_EQ(host.current_title(), QStringLiteral("Never"));

  // The host subscribes itself — no explicit raise call here.
  h.selection.select({.road = all_roads(h.document).front()});
  EXPECT_EQ(host.current_title(), QStringLiteral("Vertical Profile"));
}

// Why the dock was RENAMED rather than reusing "dock.profile": restoreState
// matches saved geometry to docks by objectName. A returning user's settings
// still carry a dock.profile entry — placed and sized for the old Profile
// panel — and reusing the name would let that stale entry capture the new,
// differently-shaped 2D Editor pane. An unknown name is ignored instead, so the
// default placement wins. (MainWindow itself needs a GL viewport and cannot be
// built offscreen, so this pins the Qt behaviour the decision rests on.)
TEST(Editor2DDock, StaleProfileLayoutDoesNotCaptureTheRenamedDock) {
  // A layout saved by the OLD build: a bottom dock called dock.profile, hidden.
  QByteArray stale_state;
  {
    QMainWindow old_window;
    old_window.setCentralWidget(new QWidget(&old_window));
    auto* old_dock = new QDockWidget(QStringLiteral("Profile"), &old_window);
    old_dock->setObjectName(QStringLiteral("dock.profile"));
    old_window.addDockWidget(Qt::LeftDockWidgetArea, old_dock); // deliberately NOT bottom
    stale_state = old_window.saveState();
  }

  // The new build: same settings blob, a dock named dock.editor2d.
  QMainWindow window;
  window.setCentralWidget(new QWidget(&window));
  auto* dock = new QDockWidget(QStringLiteral("2D Editor"), &window);
  dock->setObjectName(QStringLiteral("dock.editor2d"));
  window.addDockWidget(Qt::BottomDockWidgetArea, dock);

  window.restoreState(stale_state);

  EXPECT_EQ(window.dockWidgetArea(dock), Qt::BottomDockWidgetArea)
      << "the stale dock.profile entry must not drag the 2D Editor to the left";
}

} // namespace
} // namespace roadmaker::editor
