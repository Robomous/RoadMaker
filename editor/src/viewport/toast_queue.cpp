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
