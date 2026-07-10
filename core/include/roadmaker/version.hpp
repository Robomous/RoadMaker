#pragma once

#include <string_view>

namespace roadmaker {

/// Semantic version of the RoadMaker kernel, e.g. "0.1.0".
[[nodiscard]] std::string_view version();

} // namespace roadmaker

namespace rm = roadmaker; // NOLINT(misc-unused-alias-decls) project-wide alias
