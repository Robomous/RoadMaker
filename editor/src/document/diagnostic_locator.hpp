#pragma once

// Best-effort mapping from parser diagnostic strings to network entities.
// The kernel Diagnostic carries no entity or rule ids (a Diagnostic extension
// is planned for M2), so this parses the reader's location grammar:
//   road[N] [ /laneSection[M] [ /lane[K] ] ] [ /deeper/segments... ]
// where N and M are document-order indices and K is the OpenDRIVE lane id.
// Pure functions, no Qt.

#include "roadmaker/road/network.hpp"

#include <optional>
#include <string_view>

namespace roadmaker::editor {

struct DiagnosticTarget {
  RoadId road;
  LaneId lane; // invalid when the location names no lane
};

/// Resolves a diagnostic location to entities in `network`. Returns nullopt
/// when the string doesn't match the grammar or the indices don't resolve.
/// Caveat: road[N] is the N-th road in DOCUMENT order; roads the reader
/// skipped (missing/duplicate id) shift later indices, so resolution is
/// best-effort by design.
[[nodiscard]] std::optional<DiagnosticTarget>
resolve_diagnostic_location(const RoadNetwork& network, std::string_view location);

/// Extracts an ASAM rule id (e.g. "asam.net:xodr:1.4.0:ids.id_unique") from a
/// diagnostic message; empty view when the message cites none.
[[nodiscard]] std::string_view extract_rule_id(std::string_view message);

} // namespace roadmaker::editor
