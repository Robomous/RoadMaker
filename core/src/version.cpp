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

#include "roadmaker/version.hpp"

namespace roadmaker {

std::string_view version() {
  // RM_VERSION_STRING is injected by core/CMakeLists.txt from PROJECT_VERSION
  // so the kernel, packages, and installers can never disagree.
  return RM_VERSION_STRING;
}

} // namespace roadmaker
