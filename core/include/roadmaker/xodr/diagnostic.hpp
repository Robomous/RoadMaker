#pragma once

#include <string>
#include <vector>

namespace roadmaker {

enum class Severity {
  Info,
  Warning,
  Error,
};

/// One parser/validator finding. Parsers accumulate diagnostics and keep
/// going — input is never silently dropped; anything skipped or coerced
/// produces an entry here.
struct Diagnostic {
  Severity severity = Severity::Warning;

  /// XPath-ish location: "road[3]/planView/geometry[0]" or "road id=5".
  std::string location;

  std::string message;
};

[[nodiscard]] inline std::size_t count_errors(const std::vector<Diagnostic>& diagnostics) {
  std::size_t n = 0;
  for (const Diagnostic& d : diagnostics) {
    if (d.severity == Severity::Error) {
      ++n;
    }
  }
  return n;
}

} // namespace roadmaker
