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

// Structural guards for the chrome layout (issue #332): nothing whose width
// depends on the active tool may live in a toolbar, or the tool buttons shift
// under the cursor on every tool switch. The per-tool instruction belongs to
// the status bar's left section, where it elides instead of resizing the bar.
//
// The window is constructed but NEVER shown: ViewportWidget's GL init is
// fatal-on-failure and only runs on realize, so offscreen tests must stay
// construction-only (this is why --screenshot mode probes GL first).

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QResizeEvent>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTabBar>
#include <QToolBar>
#include <QWidget>
#include <algorithm>
#include <memory>

#include "app/elided_label.hpp"
#include "app/main_window.hpp"
#include "app/settings.hpp"
#include "app/shortcut_registry.hpp"
#include "viewport/viewport_widget.hpp"

namespace roadmaker::editor {
namespace {

class MainWindowLayoutTest : public ::testing::Test {
protected:
  MainWindow window_{nullptr, /*restore_saved_layout=*/false};
};

TEST_F(MainWindowLayoutTest, ToolbarsHoldNoVariableWidthLabels) {
  const auto bars = window_.findChildren<QToolBar*>();
  ASSERT_FALSE(bars.isEmpty());
  for (const QToolBar* bar : bars) {
    for (const QLabel* label : bar->findChildren<QLabel*>()) {
      // The one allowed label is the fixed "Template:" caption, which is shown
      // and hidden wholesale with the dropdown it labels.
      EXPECT_EQ(label->objectName(), QStringLiteral("toolOptionCaption"))
          << "toolbar " << bar->objectName().toStdString() << " holds label '"
          << label->text().toStdString() << "'";
    }
  }
}

TEST_F(MainWindowLayoutTest, ToolbarIsTwoPlainGroupedRowsWithNoTabs) {
  // #377: the category tabs are gone. The chrome is two plain top-level
  // QToolBars in the top area — the core strip (`toolbar.main`: document ops)
  // and ONE flat tool row (`toolbar.tools`: EVERY placement tool at once,
  // separator-grouped). Both being plain top-level bars, they share one left
  // origin and align by construction — no QTabBar, no page-swapping, no widget
  // nested inside a toolbar.
  auto* core = window_.findChild<QToolBar*>(QStringLiteral("toolbar.main"));
  auto* tools = window_.findChild<QToolBar*>(QStringLiteral("toolbar.tools"));
  ASSERT_NE(core, nullptr) << "the core strip is gone";
  ASSERT_NE(tools, nullptr) << "the flat tool row is gone";

  // No tabs, and none of the old nested hosting, may come back.
  EXPECT_EQ(window_.findChild<QTabBar*>(QStringLiteral("toolbar_tabs")), nullptr)
      << "the category tab bar must be gone";
  EXPECT_EQ(window_.findChild<QToolBar*>(QStringLiteral("toolbar.tabs")), nullptr)
      << "the nested tab-host toolbar must be gone";
  for (const QToolBar* bar : window_.findChildren<QToolBar*>()) {
    EXPECT_FALSE(bar->objectName().startsWith(QStringLiteral("toolbar.tab.")))
        << "a nested page toolbar '" << bar->objectName().toStdString() << "' still exists";
  }

  // The flat row must hold every placement tool from every (non-empty) tab at
  // once — not just one tab's worth — with separators grouping them. Both counts
  // come from the registry so the assertion tracks the taxonomy as pillars land.
  int expected_tools = 0;
  int largest_tab = 0;
  for (const shortcuts::ToolbarTabInfo& info : shortcuts::toolbar_tabs()) {
    int tab_tools = 0;
    for (const shortcuts::ToolbarGroupLayout& group : shortcuts::toolbar_layout(info.tab)) {
      tab_tools += static_cast<int>(group.ids.size());
    }
    expected_tools += tab_tools;
    largest_tab = std::max(largest_tab, tab_tools);
  }

  int placed = 0;
  int separators = 0;
  for (const QAction* action : tools->actions()) {
    if (action->isSeparator()) {
      ++separators;
    } else {
      ++placed;
    }
  }
  EXPECT_EQ(placed, expected_tools) << "the flat row must render every tool across all tabs";
  EXPECT_GT(expected_tools, largest_tab)
      << "the row holds more than any single tab's tools — proof it is flat, not tabbed";
  EXPECT_GT(separators, 0) << "the tool groups must be visually separated";
}

TEST_F(MainWindowLayoutTest, ToolOptionHintIsGone) {
  EXPECT_EQ(window_.findChild<QWidget*>(QStringLiteral("toolOptionHint")), nullptr);
}

TEST_F(MainWindowLayoutTest, ToolOptionsContentDoesNotTrackTheTool) {
  auto* caption = window_.findChild<QLabel*>(QStringLiteral("toolOptionCaption"));
  auto* instruction = window_.findChild<ElidedLabel*>(QStringLiteral("status_instruction"));
  ASSERT_NE(caption, nullptr);
  ASSERT_NE(instruction, nullptr);
  const QString text = caption->text();
  ASSERT_FALSE(text.isEmpty());

  // Every tool: the caption may only flip visibility, never content — while the
  // per-tool sentence does change, in the status bar where it belongs.
  static const QStringList kTools{QStringLiteral("select"),          QStringLiteral("move"),
                                  QStringLiteral("create-road"),     QStringLiteral("edit-nodes"),
                                  QStringLiteral("lane-profile"),    QStringLiteral("elevation"),
                                  QStringLiteral("create-junction"), QStringLiteral("split"),
                                  QStringLiteral("delete"),          QStringLiteral("lane-add"),
                                  QStringLiteral("lane-form"),       QStringLiteral("lane-carve"),
                                  QStringLiteral("crosswalk"),       QStringLiteral("markingPoint"),
                                  QStringLiteral("markingCurve"),    QStringLiteral("propPoint"),
                                  QStringLiteral("propCurve"),       QStringLiteral("propSpan"),
                                  QStringLiteral("propPolygon"),     QStringLiteral("corner"),
                                  QStringLiteral("stopline")};
  for (const QString& tool : kTools) {
    window_.activate_tool_for_capture(tool);
    EXPECT_EQ(caption->text(), text) << "tool " << tool.toStdString();
    EXPECT_FALSE(instruction->full_text().isEmpty()) << "tool " << tool.toStdString();
  }
}

TEST_F(MainWindowLayoutTest, InstructionSitsLeftOfThePermanentIndicators) {
  auto* instruction = window_.findChild<ElidedLabel*>(QStringLiteral("status_instruction"));
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->parentWidget(), window_.statusBar());

  // QStatusBar lays out on resize (it has no QLayout), and this window is never
  // shown, so drive one resize by hand. showMessage()'s hide/show of the normal
  // section cannot be asserted headless — QStatusBar only hides items that
  // report isVisible(), which nothing in an unshown window does.
  window_.resize(1200, 800);
  QStatusBar* bar = window_.statusBar();
  bar->resize(1200, bar->sizeHint().height());
  QResizeEvent resized(bar->size(), bar->size());
  QApplication::sendEvent(bar, &resized);
  QCoreApplication::sendPostedEvents();

  ASSERT_GT(instruction->width(), 0) << "status bar never laid out";
  for (const QLabel* other : bar->findChildren<QLabel*>()) {
    if (other == instruction) {
      continue;
    }
    EXPECT_LT(instruction->x(), other->x())
        << "the instruction owns the left edge; '" << other->text().toStdString()
        << "' must sit to its right";
    EXPECT_GT(instruction->width(), other->width()) << "the instruction takes the bar's stretch";
  }
}

TEST_F(MainWindowLayoutTest, InstructionElidesInsteadOfWidening) {
  auto* instruction = window_.findChild<ElidedLabel*>(QStringLiteral("status_instruction"));
  ASSERT_NE(instruction, nullptr);
  const QString sentence =
      QStringLiteral("Click to place polygon points, double-click or Enter to close the loop, "
                     "Esc to cancel");
  instruction->set_full_text(sentence);
  EXPECT_EQ(instruction->minimumSizeHint().width(), 0)
      << "an elided label must not impose the full text as a minimum width";

  instruction->resize(60, instruction->sizeHint().height());
  EXPECT_TRUE(instruction->displayed_text().endsWith(QChar(0x2026)));
  EXPECT_EQ(instruction->full_text(), sentence) << "the source text survives eliding";

  instruction->resize(2000, instruction->sizeHint().height());
  EXPECT_EQ(instruction->displayed_text(), sentence);
}

// #333 — View ▸ Viewport Hints. The window is constructed against a CLEARED
// QSettings scope so the assertions see the shipped default, never whatever
// the developer running the suite last toggled (and nothing leaks back out).
class ViewportHintsToggleTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Same isolation as the Settings suites: a test-only QSettings scope, so a
    // toggle asserted here can never land in the developer's real RoadMaker
    // settings (nor be read from them).
    QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RoadMakerHintsTest"));
    QSettings().clear();
    window_ = std::make_unique<MainWindow>(nullptr, /*restore_saved_layout=*/false);
    // The action is reached through its binding rather than its label, so the
    // test does not re-hardcode a key letter the registry owns (the drift this
    // whole registry exists to prevent).
    for (QAction* action : window_->findChildren<QAction*>()) {
      if (action->shortcuts() == shortcuts::sequences(shortcuts::Id::ViewportHints)) {
        toggle_ = action;
        break;
      }
    }
  }

  void TearDown() override {
    window_.reset();
    QSettings().clear();
    QCoreApplication::setOrganizationName(QStringLiteral("Robomous"));
    QCoreApplication::setApplicationName(QStringLiteral("RoadMaker"));
  }

  std::unique_ptr<MainWindow> window_;
  QAction* toggle_ = nullptr;
};

TEST_F(ViewportHintsToggleTest, DefaultsOnAndIsACheckableViewMenuEntry) {
  ASSERT_NE(toggle_, nullptr) << "no action carries the ViewportHints binding";
  EXPECT_TRUE(toggle_->isCheckable());
  EXPECT_TRUE(toggle_->isChecked());
  EXPECT_TRUE(window_->viewport()->hints_enabled());

  const QList<QAction*> menus = window_->menuBar()->actions();
  const auto view = std::find_if(menus.begin(), menus.end(), [](const QAction* action) {
    return action->menu() != nullptr && action->text().contains(QStringLiteral("View"));
  });
  ASSERT_NE(view, menus.end());
  EXPECT_TRUE((*view)->menu()->actions().contains(toggle_));
}

TEST_F(ViewportHintsToggleTest, TogglingHidesTheCardAndKeepsTheText) {
  ASSERT_NE(toggle_, nullptr);
  ViewportWidget* viewport = window_->viewport();
  window_->activate_tool_for_capture(QStringLiteral("create-road"));
  const QString hint = viewport->hint();
  ASSERT_FALSE(hint.isEmpty()) << "the active tool must feed the corner hint";
  EXPECT_TRUE(viewport->hint_visible());

  toggle_->setChecked(false);
  EXPECT_FALSE(viewport->hints_enabled());
  EXPECT_FALSE(viewport->hint_visible());
  EXPECT_EQ(viewport->hint(), hint) << "only painting is gated; the text is kept";

  toggle_->setChecked(true);
  EXPECT_TRUE(viewport->hint_visible()) << "re-enabling shows the CURRENT tool's hint at once";
}

TEST_F(ViewportHintsToggleTest, StatusBarInstructionIsUnaffected) {
  ASSERT_NE(toggle_, nullptr);
  auto* instruction = window_->findChild<ElidedLabel*>(QStringLiteral("status_instruction"));
  ASSERT_NE(instruction, nullptr);
  window_->activate_tool_for_capture(QStringLiteral("create-road"));
  const QString sentence = instruction->full_text();
  ASSERT_FALSE(sentence.isEmpty());

  toggle_->setChecked(false);
  EXPECT_EQ(instruction->full_text(), sentence);
  window_->activate_tool_for_capture(QStringLiteral("elevation"));
  EXPECT_FALSE(instruction->full_text().isEmpty())
      << "the status bar keeps tracking the tool while the viewport hint is off";
}

TEST_F(ViewportHintsToggleTest, ChoicePersistsAcrossWindows) {
  ASSERT_NE(toggle_, nullptr);
  toggle_->setChecked(false);
  EXPECT_FALSE(Settings().viewport_hints());

  MainWindow reopened(nullptr, /*restore_saved_layout=*/false);
  EXPECT_FALSE(reopened.viewport()->hints_enabled());
}

TEST(ElidedLabelTest, ElidesAndRestoresWithWidth) {
  ElidedLabel label;
  const QString sentence = QStringLiteral("A sentence long enough that no 40 pixels can hold it");
  label.set_full_text(sentence);

  label.resize(40, 20);
  EXPECT_NE(label.displayed_text(), sentence);
  EXPECT_TRUE(label.displayed_text().endsWith(QChar(0x2026)));

  label.resize(2000, 20);
  EXPECT_EQ(label.displayed_text(), sentence);
  EXPECT_EQ(label.full_text(), sentence);
}

} // namespace
} // namespace roadmaker::editor
