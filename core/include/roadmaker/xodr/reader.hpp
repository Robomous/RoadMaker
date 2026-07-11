#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace roadmaker {

/// Successful parse: the network plus everything the parser had to warn
/// about. A parse only fails outright (Expected error) on structural
/// problems — unreadable file, malformed XML, missing <OpenDRIVE> root.
/// Every element the parser skipped, coerced, or guessed at is a
/// Diagnostic, never a silent drop.
struct XodrParseResult {
  RoadNetwork network;
  std::vector<Diagnostic> diagnostics;

  /// OpenDRIVE revision from the header (e.g. 1.7); 0.0 if absent.
  double revision = 0.0;
};

/// Parses OpenDRIVE 1.4-1.9 XML from an in-memory buffer.
/// `source_name` is used in error contexts only.
[[nodiscard]] RM_API Expected<XodrParseResult>
parse_xodr(std::string_view xml_text, std::string_view source_name = "<memory>");

/// Reads and parses a .xodr file (binary mode; CRLF-safe).
[[nodiscard]] RM_API Expected<XodrParseResult> load_xodr(const std::filesystem::path& path);

} // namespace roadmaker
