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

#pragma once

// Transient toast notifications for the viewport overlay (UI-revamp Phase 1).
// Headless and fake-clock testable: the queue holds pushed messages with a
// creation time, prunes expired ones, coalesces a repeat of the most recent
// message (refreshing its timer instead of stacking a duplicate), and computes
// a fade-out opacity in the trailing window before expiry. The widget owns one
// of these and paints active(now); all timing is passed in as `now_ms`.

#include <QString>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace roadmaker::editor {

enum class ToastSeverity { Info, Success, Warning, Error };

class ToastQueue {
public:
  struct Active {
    QString text;
    ToastSeverity severity = ToastSeverity::Info;
    double opacity = 1.0; ///< 1 while live, ramping to 0 across the fade window
  };

  static constexpr std::int64_t kLifetimeMs = 4000; ///< total on-screen time
  static constexpr std::int64_t kFadeMs = 500;      ///< trailing fade-out window
  static constexpr std::size_t kMaxStack = 4;       ///< most toasts shown at once

  /// Enqueues a toast. A repeat of the current newest message (same text and
  /// severity) refreshes its timer rather than stacking a duplicate.
  void push(QString text, ToastSeverity severity, std::int64_t now_ms);

  /// Live toasts at `now_ms`, oldest first, each with its fade opacity; at most
  /// kMaxStack (oldest dropped). Prunes fully-expired entries as a side effect.
  [[nodiscard]] std::vector<Active> active(std::int64_t now_ms);

  [[nodiscard]] bool empty() const { return entries_.empty(); }

private:
  struct Entry {
    QString text;
    ToastSeverity severity;
    std::int64_t created_ms;
  };

  std::vector<Entry> entries_;
};

} // namespace roadmaker::editor
