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

#include "viewport/toast_queue.hpp"

#include <algorithm>

namespace roadmaker::editor {

void ToastQueue::push(QString text, ToastSeverity severity, std::int64_t now_ms) {
  if (!entries_.empty()) {
    Entry& newest = entries_.back();
    if (newest.text == text && newest.severity == severity) {
      newest.created_ms = now_ms; // coalesce: refresh the timer, no duplicate
      return;
    }
  }
  entries_.push_back(Entry{.text = std::move(text), .severity = severity, .created_ms = now_ms});
}

std::vector<ToastQueue::Active> ToastQueue::active(std::int64_t now_ms) {
  // Drop fully-expired entries (created before the lifetime window).
  std::erase_if(entries_,
                [now_ms](const Entry& entry) { return now_ms - entry.created_ms >= kLifetimeMs; });

  std::vector<Active> result;
  result.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    const std::int64_t age = now_ms - entry.created_ms;
    const std::int64_t fade_start = kLifetimeMs - kFadeMs;
    const double opacity =
        age <= fade_start ? 1.0
                          : static_cast<double>(kLifetimeMs - age) / static_cast<double>(kFadeMs);
    result.push_back(Active{
        .text = entry.text, .severity = entry.severity, .opacity = std::clamp(opacity, 0.0, 1.0)});
  }

  // Cap the stack to the most recent kMaxStack (drop the oldest).
  if (result.size() > kMaxStack) {
    result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(kMaxStack));
  }
  return result;
}

} // namespace roadmaker::editor
