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

#include "render/gl_functions.hpp"

namespace roadmaker::editor::gl {

// NOLINTBEGIN(readability-identifier-naming)
#define RM_GL_DEFINE(ret, name, ...) ret(RM_GL_APIENTRY* name)(__VA_ARGS__) = nullptr;
RM_GL_FUNCTIONS(RM_GL_DEFINE)
#undef RM_GL_DEFINE

// NOLINTEND(readability-identifier-naming)

bool load_functions(ProcResolver resolver) {
  if (resolver == nullptr) {
    return false;
  }
  bool ok = true;
#define RM_GL_LOAD(ret, name, ...)                                                                 \
  name = reinterpret_cast<ret(RM_GL_APIENTRY*)(__VA_ARGS__)>(resolver("gl" #name));                \
  ok = ok && (name != nullptr);
  RM_GL_FUNCTIONS(RM_GL_LOAD)
#undef RM_GL_LOAD
  return ok;
}

} // namespace roadmaker::editor::gl
