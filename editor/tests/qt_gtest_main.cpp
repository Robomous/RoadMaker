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
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
