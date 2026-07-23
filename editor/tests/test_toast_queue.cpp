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

#include <gtest/gtest.h>

#include "viewport/toast_queue.hpp"

namespace roadmaker::editor {
namespace {

TEST(ToastQueue, ShowsAPushedToastUntilItExpires) {
  ToastQueue queue;
  queue.push(QStringLiteral("saved"), ToastSeverity::Success, 0);
  ASSERT_EQ(queue.active(0).size(), 1U);
  EXPECT_EQ(queue.active(0)[0].text, "saved");
  EXPECT_DOUBLE_EQ(queue.active(0)[0].opacity, 1.0);
  EXPECT_EQ(queue.active(ToastQueue::kLifetimeMs - 1).size(), 1U);
  EXPECT_TRUE(queue.active(ToastQueue::kLifetimeMs).empty());
}

TEST(ToastQueue, FadesOutInTheTrailingWindow) {
  ToastQueue queue;
  queue.push(QStringLiteral("hi"), ToastSeverity::Info, 0);
  const std::int64_t fade_start = ToastQueue::kLifetimeMs - ToastQueue::kFadeMs;
  EXPECT_DOUBLE_EQ(queue.active(fade_start)[0].opacity, 1.0);
  EXPECT_NEAR(queue.active(fade_start + (ToastQueue::kFadeMs / 2))[0].opacity, 0.5, 1e-9);
}

TEST(ToastQueue, CoalescesARepeatOfTheNewestMessage) {
  ToastQueue queue;
  queue.push(QStringLiteral("same"), ToastSeverity::Warning, 0);
  queue.push(QStringLiteral("same"), ToastSeverity::Warning, 1000); // refresh, no duplicate
  EXPECT_EQ(queue.active(1000).size(), 1U);
  // The refresh extends its life past the original expiry.
  EXPECT_EQ(queue.active(ToastQueue::kLifetimeMs + 500).size(), 1U);
}

TEST(ToastQueue, StacksDistinctToastsOldestFirstCappedToMax) {
  ToastQueue queue;
  for (int i = 0; i < 6; ++i) {
    queue.push(QString::number(i), ToastSeverity::Info, i);
  }
  const auto active = queue.active(6);
  ASSERT_EQ(active.size(), ToastQueue::kMaxStack);
  EXPECT_EQ(active.front().text, QString::number(6 - static_cast<int>(ToastQueue::kMaxStack)));
  EXPECT_EQ(active.back().text, "5");
}

} // namespace
} // namespace roadmaker::editor
