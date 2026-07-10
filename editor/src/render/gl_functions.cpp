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
