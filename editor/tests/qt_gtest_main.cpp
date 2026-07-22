// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

// GoogleTest remains the runner (project testing standard — Qt Test is linked
// ONLY for QSignalSpy/QAbstractItemModelTester helpers). A QApplication must
// exist for QWidget/model code, and the offscreen platform keeps the suite
// headless on CI.

#include <gtest/gtest.h>

#include <QApplication>

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
