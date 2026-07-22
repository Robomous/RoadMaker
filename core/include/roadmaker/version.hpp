// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"

#include <string_view>

namespace roadmaker {

/// Semantic version of the RoadMaker kernel, e.g. "0.0.1".
[[nodiscard]] RM_API std::string_view version();

} // namespace roadmaker

namespace rm = roadmaker; // NOLINT(misc-unused-alias-decls) project-wide alias
