// The slot's drop contract in isolation: it accepts exactly the Library's MIME
// type, republishes the dragged key, and asks to be taken to its category. What
// a key MEANS is the consumer's business (test_panels.cpp covers the Model slot
// committing set_object_model).

#include <gtest/gtest.h>

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>

#include "document/library_list_model.hpp"
#include "panels/slot_widget.hpp"

namespace roadmaker::editor {
namespace {

/// A drag payload shaped exactly like LibraryListModel::mimeData's.
QMimeData* library_mime(const QByteArray& key) {
  auto* mime = new QMimeData;
  mime->setData(QString::fromLatin1(kLibraryItemMimeType), key);
  return mime;
}

/// Sends enter+drop of `mime` onto `slot`, as a real drag would.
void drop_on(SlotWidget& slot, QMimeData* mime) {
  const QPointF pos(10, 10);
  QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&slot, &enter);
  QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&slot, &drop);
}

TEST(SlotWidget, AcceptsALibraryDragAndRepublishesTheKey) {
  SlotWidget slot(QStringLiteral("Props"));
  QSignalSpy dropped(&slot, &SlotWidget::item_dropped);

  QMimeData* mime = library_mime("tree_pine");
  drop_on(slot, mime);

  ASSERT_EQ(dropped.count(), 1);
  EXPECT_EQ(dropped.at(0).at(0).toString(), QStringLiteral("tree_pine"));
  delete mime;
}

TEST(SlotWidget, IgnoresADragThatIsNotALibraryItem) {
  SlotWidget slot(QStringLiteral("Props"));
  QSignalSpy dropped(&slot, &SlotWidget::item_dropped);

  auto* mime = new QMimeData;
  mime->setText(QStringLiteral("some dragged text"));
  const QPointF pos(10, 10);
  QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&slot, &enter);
  EXPECT_FALSE(enter.isAccepted()) << "an unaccepted enter lets the drag fall through";

  QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&slot, &drop);
  EXPECT_EQ(dropped.count(), 0);
  delete mime;
}

TEST(SlotWidget, IgnoresAnEmptyPayload) {
  SlotWidget slot(QStringLiteral("Props"));
  QSignalSpy dropped(&slot, &SlotWidget::item_dropped);
  QMimeData* mime = library_mime("");
  drop_on(slot, mime);
  EXPECT_EQ(dropped.count(), 0) << "a malformed payload is not a drop";
  delete mime;
}

// The hover affordance: the slot must look like it will take the drop before
// the user commits to letting go.
TEST(SlotWidget, ShowsAHoverAffordanceWhileADroppableItemIsOverIt) {
  SlotWidget slot(QStringLiteral("Props"));
  EXPECT_TRUE(slot.styleSheet().isEmpty()) << "resting slot carries no accent";

  QMimeData* mime = library_mime("tree_pine");
  const QPointF pos(10, 10);
  QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&slot, &enter);
  EXPECT_TRUE(enter.isAccepted());
  EXPECT_FALSE(slot.styleSheet().isEmpty()) << "hovered slot is accented";

  QDragLeaveEvent leave;
  QCoreApplication::sendEvent(&slot, &leave);
  EXPECT_TRUE(slot.styleSheet().isEmpty()) << "the affordance must not outlive the drag";
  delete mime;
}

TEST(SlotWidget, DropClearsTheHoverAffordance) {
  SlotWidget slot(QStringLiteral("Props"));
  QMimeData* mime = library_mime("tree_pine");
  drop_on(slot, mime);
  EXPECT_TRUE(slot.styleSheet().isEmpty());
  delete mime;
}

TEST(SlotWidget, ClickingAsksToBeTakenToItsLibraryCategory) {
  SlotWidget slot(QStringLiteral("Props"));
  slot.resize(120, 60);
  QSignalSpy engaged(&slot, &SlotWidget::engage_requested);

  QTest::mouseClick(&slot, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));

  ASSERT_EQ(engaged.count(), 1);
  EXPECT_EQ(engaged.at(0).at(0).toString(), QStringLiteral("Props"));
}

TEST(SlotWidget, ShowsThePlaceholderUntilAnItemIsSet) {
  SlotWidget slot(QStringLiteral("Props"));
  const auto caption = [&slot] {
    return slot.findChildren<QLabel*>().at(1)->text(); // [0] preview, [1] caption
  };
  EXPECT_EQ(caption(), QStringLiteral("(none)"));

  slot.set_item(QStringLiteral("tree_pine"));
  EXPECT_EQ(caption(), QStringLiteral("tree_pine"));

  slot.set_item({}); // cleared again
  EXPECT_EQ(caption(), QStringLiteral("(none)"));
}

} // namespace
} // namespace roadmaker::editor
