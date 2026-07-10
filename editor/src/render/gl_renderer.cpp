#include "render/gl_renderer.hpp"

#include <spdlog/spdlog.h>

#include <string>

#include "render/gl_functions.hpp"

namespace roadmaker::editor {

namespace {

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
uniform mat4 u_view;
uniform mat4 u_projection;
out vec3 v_normal;
void main() {
  v_normal = in_normal;
  gl_Position = u_projection * u_view * vec4(in_position, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 v_normal;
uniform vec4 u_color;
uniform float u_highlight; // 0 = normal, 1 = selected
uniform float u_lit;       // 0 = unlit (lines), 1 = lambert
out vec4 frag_color;
void main() {
  vec3 light_dir = normalize(vec3(0.35, 0.25, 0.9));
  float lambert = max(dot(normalize(v_normal), light_dir), 0.0);
  float shade = mix(1.0, 0.35 + 0.65 * lambert, u_lit);
  vec3 base = u_color.rgb * shade;
  vec3 highlight_tint = vec3(1.0, 0.65, 0.1);
  frag_color = vec4(mix(base, highlight_tint, 0.55 * u_highlight), u_color.a);
}
)";

std::uint32_t compile(gl::GLenum type, const char* source) {
  const gl::GLuint shader = gl::CreateShader(type);
  gl::ShaderSource(shader, 1, &source, nullptr);
  gl::CompileShader(shader);
  gl::GLint status = 0;
  gl::GetShaderiv(shader, gl::kCompileStatus, &status);
  if (status == 0) {
    std::string log(512, '\0');
    gl::GetShaderInfoLog(shader, static_cast<gl::GLsizei>(log.size()), nullptr, log.data());
    spdlog::error("shader compile failed: {}", log.c_str());
    gl::DeleteShader(shader);
    return 0;
  }
  return shader;
}

} // namespace

GLRenderer::~GLRenderer() {
  shutdown();
}

bool GLRenderer::init() {
  // Precondition: gl::load_functions(resolver) succeeded with the current
  // context — the windowing layer owns the resolver, not the renderer.
  const std::uint32_t vs = compile(gl::kVertexShader, kVertexShader);
  const std::uint32_t fs = compile(gl::kFragmentShader, kFragmentShader);
  if (vs == 0 || fs == 0) {
    return false;
  }
  program_ = gl::CreateProgram();
  gl::AttachShader(program_, vs);
  gl::AttachShader(program_, fs);
  gl::LinkProgram(program_);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  gl::GLint status = 0;
  gl::GetProgramiv(program_, gl::kLinkStatus, &status);
  if (status == 0) {
    std::string log(512, '\0');
    gl::GetProgramInfoLog(program_, static_cast<gl::GLsizei>(log.size()), nullptr, log.data());
    spdlog::error("program link failed: {}", log.c_str());
    return false;
  }
  u_view_ = gl::GetUniformLocation(program_, "u_view");
  u_projection_ = gl::GetUniformLocation(program_, "u_projection");
  u_color_ = gl::GetUniformLocation(program_, "u_color");
  u_highlight_ = gl::GetUniformLocation(program_, "u_highlight");
  u_lit_ = gl::GetUniformLocation(program_, "u_lit");
  ready_ = true;
  return true;
}

void GLRenderer::shutdown() {
  if (!ready_) {
    return;
  }
  clear_meshes();
  gl::DeleteProgram(program_);
  program_ = 0;
  ready_ = false;
}

RenderMeshHandle GLRenderer::upload(const RenderMeshData& data) {
  if (!ready_ || data.positions.empty() || data.indices.empty()) {
    return {};
  }
  GpuMesh mesh;
  mesh.color = data.color;
  mesh.kind = data.kind;
  mesh.index_count = static_cast<std::int32_t>(data.indices.size());

  // Interleave position + normal (lines get zero normals).
  const std::size_t vertex_count = data.positions.size() / 3;
  std::vector<float> interleaved;
  interleaved.reserve(vertex_count * 6);
  const bool has_normals = data.normals.size() == data.positions.size();
  for (std::size_t i = 0; i < vertex_count; ++i) {
    interleaved.push_back(data.positions[(i * 3) + 0]);
    interleaved.push_back(data.positions[(i * 3) + 1]);
    interleaved.push_back(data.positions[(i * 3) + 2]);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 0] : 0.0F);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 1] : 0.0F);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 2] : 1.0F);
  }

  gl::GenVertexArrays(1, &mesh.vao);
  gl::BindVertexArray(mesh.vao);
  gl::GenBuffers(1, &mesh.vbo);
  gl::BindBuffer(gl::kArrayBuffer, mesh.vbo);
  gl::BufferData(gl::kArrayBuffer,
                 static_cast<gl::GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(),
                 gl::kStaticDraw);
  gl::GenBuffers(1, &mesh.ebo);
  gl::BindBuffer(gl::kElementArrayBuffer, mesh.ebo);
  gl::BufferData(gl::kElementArrayBuffer,
                 static_cast<gl::GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(),
                 gl::kStaticDraw);

  const auto stride = static_cast<gl::GLsizei>(6 * sizeof(float));
  gl::EnableVertexAttribArray(0);
  gl::VertexAttribPointer(0, 3, gl::kFloat, 0, stride, nullptr);
  gl::EnableVertexAttribArray(1);
  gl::VertexAttribPointer(
      1, 3, gl::kFloat, 0, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
  gl::BindVertexArray(0);

  const RenderMeshHandle handle{next_id_++};
  meshes_.emplace(handle.id, mesh);
  return handle;
}

void GLRenderer::destroy(GpuMesh& mesh) {
  gl::DeleteBuffers(1, &mesh.vbo);
  gl::DeleteBuffers(1, &mesh.ebo);
  gl::DeleteVertexArrays(1, &mesh.vao);
}

void GLRenderer::remove(RenderMeshHandle handle) {
  const auto found = meshes_.find(handle.id);
  if (found != meshes_.end()) {
    destroy(found->second);
    meshes_.erase(found);
  }
}

void GLRenderer::clear_meshes() {
  for (auto& [id, mesh] : meshes_) {
    destroy(mesh);
  }
  meshes_.clear();
}

void GLRenderer::render(const std::vector<DrawItem>& items,
                        const CameraMatrices& camera,
                        int width,
                        int height) {
  if (!ready_ || width <= 0 || height <= 0) {
    return;
  }
  gl::Viewport(0, 0, width, height);
  gl::ClearColor(0.13F, 0.14F, 0.16F, 1.0F);
  gl::Clear(gl::kColorBufferBit | gl::kDepthBufferBit);
  gl::Enable(gl::kDepthTest);
  gl::DepthFunc(gl::kLess);

  gl::UseProgram(program_);
  gl::UniformMatrix4fv(u_view_, 1, 0, camera.view.data());
  gl::UniformMatrix4fv(u_projection_, 1, 0, camera.projection.data());

  for (const DrawItem& item : items) {
    const auto found = meshes_.find(item.mesh.id);
    if (found == meshes_.end()) {
      continue;
    }
    const GpuMesh& mesh = found->second;
    gl::Uniform4f(u_color_, mesh.color[0], mesh.color[1], mesh.color[2], mesh.color[3]);
    gl::Uniform4f(u_highlight_, item.highlighted ? 1.0F : 0.0F, 0, 0, 0);
    gl::Uniform4f(u_lit_, mesh.kind == PrimitiveKind::Triangles ? 1.0F : 0.0F, 0, 0, 0);
    gl::BindVertexArray(mesh.vao);
    gl::DrawElements(mesh.kind == PrimitiveKind::Triangles ? gl::kTriangles : gl::kLines,
                     mesh.index_count,
                     gl::kUnsignedInt,
                     nullptr);
  }
  gl::BindVertexArray(0);
  gl::UseProgram(0);
}

} // namespace roadmaker::editor
