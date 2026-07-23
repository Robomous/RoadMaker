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
