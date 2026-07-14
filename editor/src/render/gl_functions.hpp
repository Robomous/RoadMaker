#pragma once

// Minimal self-contained OpenGL 3.3 core function loader. We deliberately
// avoid glad/GLEW: the editor needs ~30 entry points, all resolved through a
// caller-supplied resolver (QOpenGLContext::getProcAddress in the Qt editor),
// which works identically on macOS, Linux, and Windows (on Windows, functions
// beyond GL 1.1 are not exported by opengl32.dll — that is WHY this loader
// exists). Keeping the resolver injected keeps this file toolkit-agnostic.
//
// Only editor/src/render/ and the viewport widget may include this header
// (architecture rule).

#include <cstddef>
#include <cstdint>

namespace roadmaker::editor::gl {

using GLenum = std::uint32_t;
using GLuint = std::uint32_t;
using GLint = std::int32_t;
using GLsizei = std::int32_t;
using GLboolean = std::uint8_t;
using GLbitfield = std::uint32_t;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

// Tokens (from the GL 3.3 core spec).
inline constexpr GLenum kColorBufferBit = 0x00004000;
inline constexpr GLenum kDepthBufferBit = 0x00000100;
inline constexpr GLenum kDepthTest = 0x0B71;
inline constexpr GLenum kLess = 0x0201;
inline constexpr GLenum kTriangles = 0x0004;
inline constexpr GLenum kTriangleStrip = 0x0005;
inline constexpr GLenum kLines = 0x0001;
inline constexpr GLenum kBlend = 0x0BE2;
inline constexpr GLenum kSrcAlpha = 0x0302;
inline constexpr GLenum kOneMinusSrcAlpha = 0x0303;
inline constexpr GLenum kUnsignedInt = 0x1405;
inline constexpr GLenum kFloat = 0x1406;
inline constexpr GLenum kArrayBuffer = 0x8892;
inline constexpr GLenum kElementArrayBuffer = 0x8893;
inline constexpr GLenum kStaticDraw = 0x88E4;
inline constexpr GLenum kVertexShader = 0x8B31;
inline constexpr GLenum kFragmentShader = 0x8B30;
inline constexpr GLenum kCompileStatus = 0x8B81;
inline constexpr GLenum kLinkStatus = 0x8B82;
inline constexpr GLenum kPolygonOffsetFill = 0x8037;
inline constexpr GLenum kTexture2D = 0x0DE1;
inline constexpr GLenum kTexture0 = 0x84C0;
inline constexpr GLenum kRgba = 0x1908;
inline constexpr GLenum kRgba8 = 0x8058;
inline constexpr GLenum kUnsignedByte = 0x1401;
inline constexpr GLenum kTextureMinFilter = 0x2801;
inline constexpr GLenum kTextureMagFilter = 0x2800;
inline constexpr GLenum kTextureWrapS = 0x2802;
inline constexpr GLenum kTextureWrapT = 0x2803;
inline constexpr GLenum kLinear = 0x2601;
inline constexpr GLenum kLinearMipmapLinear = 0x2703;
inline constexpr GLenum kRepeat = 0x2901;

// Function pointers, loaded by load_functions().
// NOLINTBEGIN(readability-identifier-naming) — official GL spellings.
#if defined(_WIN32)
#define RM_GL_APIENTRY __stdcall
#else
#define RM_GL_APIENTRY
#endif

#define RM_GL_FUNCTIONS(X)                                                                         \
  X(void, Enable, GLenum)                                                                          \
  X(void, Disable, GLenum)                                                                         \
  X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                          \
  X(void, Clear, GLbitfield)                                                                       \
  X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                                \
  X(void, DepthFunc, GLenum)                                                                       \
  X(void, DepthMask, GLboolean)                                                                    \
  X(void, BlendFunc, GLenum, GLenum)                                                               \
  X(void, LineWidth, GLfloat)                                                                      \
  X(void, PolygonOffset, GLfloat, GLfloat)                                                         \
  X(GLuint, CreateShader, GLenum)                                                                  \
  X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                       \
  X(void, CompileShader, GLuint)                                                                   \
  X(void, GetShaderiv, GLuint, GLenum, GLint*)                                                     \
  X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                    \
  X(GLuint, CreateProgram)                                                                         \
  X(void, AttachShader, GLuint, GLuint)                                                            \
  X(void, LinkProgram, GLuint)                                                                     \
  X(void, GetProgramiv, GLuint, GLenum, GLint*)                                                    \
  X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                   \
  X(void, DeleteShader, GLuint)                                                                    \
  X(void, DeleteProgram, GLuint)                                                                   \
  X(void, UseProgram, GLuint)                                                                      \
  X(GLint, GetUniformLocation, GLuint, const GLchar*)                                              \
  X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                             \
  X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                    \
  X(void, Uniform3f, GLint, GLfloat, GLfloat, GLfloat)                                             \
  X(void, Uniform1f, GLint, GLfloat)                                                               \
  X(void, GenVertexArrays, GLsizei, GLuint*)                                                       \
  X(void, BindVertexArray, GLuint)                                                                 \
  X(void, DeleteVertexArrays, GLsizei, const GLuint*)                                              \
  X(void, GenBuffers, GLsizei, GLuint*)                                                            \
  X(void, BindBuffer, GLenum, GLuint)                                                              \
  X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum)                                     \
  X(void, DeleteBuffers, GLsizei, const GLuint*)                                                   \
  X(void, EnableVertexAttribArray, GLuint)                                                         \
  X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)             \
  X(void, DrawElements, GLenum, GLsizei, GLenum, const void*)                                      \
  X(void, DrawArrays, GLenum, GLint, GLsizei)                                                      \
  X(void, Uniform1i, GLint, GLint)                                                                 \
  X(void, ActiveTexture, GLenum)                                                                   \
  X(void, GenTextures, GLsizei, GLuint*)                                                           \
  X(void, BindTexture, GLenum, GLuint)                                                             \
  X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)  \
  X(void, TexParameteri, GLenum, GLenum, GLint)                                                    \
  X(void, GenerateMipmap, GLenum)                                                                  \
  X(void, DeleteTextures, GLsizei, const GLuint*)

#define RM_GL_DECLARE(ret, name, ...) extern ret(RM_GL_APIENTRY* name)(__VA_ARGS__);
RM_GL_FUNCTIONS(RM_GL_DECLARE)
#undef RM_GL_DECLARE
// NOLINTEND(readability-identifier-naming)

/// Resolves a GL entry point by name ("glDrawElements") with a current GL
/// context, e.g. QOpenGLContext::currentContext()->getProcAddress.
using ProcResolver = void* (*)(const char* name);

/// Resolves every pointer above through `resolver`. Call once with a current
/// GL context. Returns false if any function is missing.
[[nodiscard]] bool load_functions(ProcResolver resolver);

} // namespace roadmaker::editor::gl
