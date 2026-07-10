#pragma once

#include "roadmaker/road/id.hpp"

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

  /// ASAM OpenDRIVE checker-rule UID ("asam.net:xodr:<version>:<rule>",
  /// see roadmaker/xodr/rules.hpp); empty when no normative rule applies
  /// (tool limitations, schema-level defects).
  std::string rule_id;

  /// Entities the finding concerns, resolvable via RoadNetwork::road()/
  /// lane(). Invalid when the entity was never created (e.g. a skipped
  /// duplicate) or the finding is document-scoped.
  RoadId road;
  LaneId lane;
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
