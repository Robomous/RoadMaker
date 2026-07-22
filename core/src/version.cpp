// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/version.hpp"

namespace roadmaker {

std::string_view version() {
  // RM_VERSION_STRING is injected by core/CMakeLists.txt from PROJECT_VERSION
  // so the kernel, packages, and installers can never disagree.
  return RM_VERSION_STRING;
}

} // namespace roadmaker
