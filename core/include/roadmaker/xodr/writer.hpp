#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/road/network.hpp"

#include <filesystem>
#include <string>

namespace roadmaker {

/// Serializes the network as OpenDRIVE 1.7 XML.
///
/// Validates before writing — a network that would produce invalid
/// OpenDRIVE is refused (Error, code InvalidArgument) rather than written:
///   - every road has plan-view geometry and at least one lane section
///   - geometry records are contiguous (G1 position/heading within rm::tol)
///   - lane links reference lanes that exist in the neighboring section
///
/// Output is deterministic (no timestamps) so round-trip tests and version
/// control stay stable.
[[nodiscard]] Expected<std::string> write_xodr(const RoadNetwork& network,
                                               std::string_view document_name = "roadmaker");

/// write_xodr + save to disk (binary mode, '\n' line endings).
[[nodiscard]] Expected<void> save_xodr(const RoadNetwork& network,
                                       const std::filesystem::path& path,
                                       std::string_view document_name = "roadmaker");

} // namespace roadmaker
