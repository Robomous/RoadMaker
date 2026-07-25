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

// GoogleTest remains the runner (project testing standard — Qt Test is linked
// ONLY for QSignalSpy/QAbstractItemModelTester helpers). A QApplication must
// exist for QWidget/model code, and the offscreen platform keeps the suite
// headless on CI.

#include <gtest/gtest.h>

#include <QApplication>

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  // QSettings resolves its store from the organization/application names, and
  // without these it falls back to the executable name — which on some
  // platforms lands in, or next to, the shipped RoadMaker store. Any case that
  // forgets the per-suite isolation preamble would then edit the DEVELOPER's
  // real settings; the recent list is destroyed exactly that way (#399). These
  // are the process-wide floor: suites still rename the application per test
  // for parallel safety, and restore THESE names, never the shipped ones.
  QCoreApplication::setOrganizationName(QStringLiteral("RobomousTests"));
  QCoreApplication::setApplicationName(QStringLiteral("RoadMakerEditorTests"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
