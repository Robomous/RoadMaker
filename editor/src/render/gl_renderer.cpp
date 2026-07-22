// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "render/gl_renderer.hpp"

#include <spdlog/spdlog.h>

#include <string>

#include "render/gl_functions.hpp"

namespace roadmaker::editor {

namespace {

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
// Per-instance model matrix (attrib divisor 1), the four columns of a
// column-major mat4. Only read when u_use_instancing == 1.
layout(location = 3) in vec4 in_model_0;
layout(location = 4) in vec4 in_model_1;
layout(location = 5) in vec4 in_model_2;
layout(location = 6) in vec4 in_model_3;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform mat4 u_model; // single-draw transform; identity for non-instanced draws
uniform int u_use_instancing; // 1 = per-instance attribs, 0 = u_model
out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world_pos;
void main() {
  // Uniform-controlled switch keeps the non-instanced path bit-identical (it
  // still runs through u_model); instanced draws build the matrix from the
  // per-instance columns. Rigid transforms → mat3(model) stays valid for normals.
  mat4 model = u_use_instancing == 1
      ? mat4(in_model_0, in_model_1, in_model_2, in_model_3)
      : u_model;
  vec4 world = model * vec4(in_position, 1.0);
  v_world_pos = world.xyz;
  v_normal = mat3(model) * in_normal;
  v_uv = in_uv;
  gl_Position = u_projection * u_view * world;
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_world_pos;
uniform vec4 u_color;
uniform float u_highlight;  // accent mix strength: 0 none, hover < selected
uniform vec3 u_accent;      // theme accent token (hover/selection emphasis)
uniform float u_lit;        // 0 = unlit (lines), 1 = lit
uniform int u_has_texture;  // 1 = sample u_base_color, 0 = flat u_color
uniform sampler2D u_base_color;
uniform float u_uv_scale;   // texels per meter (material tiling)
uniform vec4 u_tint;        // multiplies the texture sample
// PBR-lite maps (design doc §5). Absent → the fallbacks keep a mapless material
// pixel-identical to before: no normal map = geometric normal, no roughness map
// = the u_roughness scalar. A default Material has u_roughness = 1 (fully rough)
// so the specular lobe below scales by (1 - rough) = 0 and adds nothing.
uniform int u_has_normal;
uniform sampler2D u_normal;     // tangent-space, GL (Y+) convention
uniform float u_normal_strength;
uniform int u_has_roughness;
uniform sampler2D u_roughness_map; // .r channel
uniform float u_roughness;         // scalar fallback / modulator
uniform vec3 u_camera_pos;      // world-space eye (specular half-vector)
// Environment lighting (hemisphere ambient + one directional sun). The Sober
// preset sets sky == ground == white and intensity 0.65 so this reduces to the
// old 0.35 + 0.65*lambert grey shading exactly.
uniform vec3 u_sun_dir;
uniform vec3 u_sun_color;
uniform float u_sun_intensity;
uniform vec3 u_sky_light;
uniform vec3 u_ground_light;
uniform float u_ambient;
out vec4 frag_color;

// Per-fragment cotangent frame from screen-space derivatives (design doc §5:
// no vertex-format/tangent change). Stable for the planar-UV road/deck geometry
// this milestone maps.
vec3 perturb_normal(vec3 geom_n, vec2 uv) {
  vec3 dp1 = dFdx(v_world_pos);
  vec3 dp2 = dFdy(v_world_pos);
  vec2 duv1 = dFdx(uv);
  vec2 duv2 = dFdy(uv);
  vec3 dp2perp = cross(dp2, geom_n);
  vec3 dp1perp = cross(geom_n, dp1);
  vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
  vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
  float inv_max = inversesqrt(max(dot(t, t), dot(b, b)));
  mat3 tbn = mat3(t * inv_max, b * inv_max, geom_n);
  vec3 sample_n = texture(u_normal, uv).xyz * 2.0 - 1.0;
  sample_n.xy *= u_normal_strength;
  return normalize(tbn * sample_n);
}

void main() {
  vec2 uv = v_uv * u_uv_scale;
  // A default Material carries no texture, so u_has_texture stays 0 and the
  // flat u_color path is used.
  vec4 albedo = u_has_texture == 1 ? texture(u_base_color, uv) * u_tint : u_color;
  vec3 geom_n = normalize(v_normal);
  vec3 n = u_has_normal == 1 ? perturb_normal(geom_n, uv) : geom_n;
  float rough =
      u_has_roughness == 1 ? texture(u_roughness_map, uv).r * u_roughness : u_roughness;

  vec3 hemi = mix(u_ground_light, u_sky_light, 0.5 * (n.z + 1.0)) * u_ambient;
  vec3 sun_dir = normalize(u_sun_dir);
  float lambert = max(dot(n, sun_dir), 0.0);
  vec3 direct = u_sun_color * (u_sun_intensity * lambert);
  // Sun-only Blinn-Phong lobe scaled by (1 - rough): zero at the default full
  // roughness, so a mapless/default material is unchanged. Gated on u_lit so
  // lines (and the Sober-flat path via rough=1) never gain a highlight.
  float gloss = 1.0 - rough;
  vec3 half_v = normalize(sun_dir + normalize(u_camera_pos - v_world_pos));
  float spec = pow(max(dot(n, half_v), 0.0), mix(8.0, 96.0, gloss)) * gloss;
  direct += u_sun_color * (u_sun_intensity * spec);

  vec3 lighting = hemi + direct;                // colored light multiplier
  vec3 shade = mix(vec3(1.0), lighting, u_lit); // lines (u_lit=0) stay unlit
  vec3 base = albedo.rgb * shade;
  frag_color = vec4(mix(base, u_accent, u_highlight), albedo.a);
}
)";

// Fullscreen sky gradient — attributeless triangle, drawn first with depth
// untouched. Screen-space vertical mix: horizon color low, sky color high.
constexpr const char* kSkyVertexShader = R"(#version 330 core
out vec2 v_uv;
void main() {
  vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = pos;
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kSkyFragmentShader = R"(#version 330 core
in vec2 v_uv;
uniform vec3 u_sky_top;
uniform vec3 u_sky_horizon;
out vec4 frag_color;
void main() {
  float h = smoothstep(0.0, 0.8, v_uv.y);
  frag_color = vec4(mix(u_sky_horizon, u_sky_top, h), 1.0);
}
)";

// Ground grid — a camera-following quad on z=0, lines computed procedurally
// (1 m minor / 10 m major, fwidth-antialiased) and faded with distance so
// the plane dissolves toward the horizon instead of ending at a hard edge.
// The x/y origin axes tint their zero lines. Alpha-blended over the sky,
// depth writes off (roads at z>=0 draw over it afterwards).
constexpr const char* kGridVertexShader = R"(#version 330 core
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_eye;
out vec2 v_world;
const float kExtent = 4000.0;
void main() {
  vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) - 1.0;
  vec2 world = u_eye.xy + corner * kExtent;
  v_world = world;
  gl_Position = u_projection * u_view * vec4(world, 0.0, 1.0);
}
)";

constexpr const char* kGridFragmentShader = R"(#version 330 core
in vec2 v_world;
uniform vec3 u_eye;
uniform vec4 u_major;
uniform vec4 u_minor;
uniform vec3 u_axis_x;
uniform vec3 u_axis_y;
out vec4 frag_color;

float grid_line(vec2 p, float spacing) {
  vec2 q = p / spacing;
  vec2 g = abs(fract(q - 0.5) - 0.5) / fwidth(q);
  return 1.0 - min(min(g.x, g.y), 1.0);
}

void main() {
  float dist = distance(v_world, u_eye.xy);
  float eye_height = max(u_eye.z, 4.0);
  float fade_major = exp(-dist / (eye_height * 14.0));
  float fade_minor = exp(-dist / (eye_height * 5.0));

  vec4 color = vec4(0.0);
  float minor = grid_line(v_world, 1.0);
  color = mix(color, vec4(u_minor.rgb, u_minor.a * fade_minor), minor);
  float major = grid_line(v_world, 10.0);
  color = mix(color, vec4(u_major.rgb, u_major.a * fade_major), major);

  float x_axis = 1.0 - min(abs(v_world.y) / fwidth(v_world.y), 1.0);
  float y_axis = 1.0 - min(abs(v_world.x) / fwidth(v_world.x), 1.0);
  color = mix(color, vec4(u_axis_x, min(u_major.a * fade_major * 1.6, 1.0)), x_axis);
  color = mix(color, vec4(u_axis_y, min(u_major.a * fade_major * 1.6, 1.0)), y_axis);

  if (color.a <= 0.004) {
    discard;
  }
  frag_color = color;
}
)";

// Procedural grass ground — a large camera-following quad at a fixed world
// height (the network floor), drawn OPAQUE with depth writes so the road
// network occludes it. Textured mode only; replaces the reference grid as the
// scene's base. Same camera-following trick as the grid, but with depth.
constexpr const char* kGroundVertexShader = R"(#version 330 core
uniform mat4 u_view;
uniform mat4 u_projection;
uniform vec3 u_eye;
uniform float u_base_z;
out vec2 v_world;
const float kExtent = 6000.0;
void main() {
  vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) - 1.0;
  vec2 world = u_eye.xy + corner * kExtent;
  v_world = world;
  gl_Position = u_projection * u_view * vec4(world, u_base_z, 1.0);
}
)";

constexpr const char* kGroundFragmentShader = R"(#version 330 core
in vec2 v_world;
uniform vec3 u_eye;
uniform vec3 u_horizon;        // sky horizon color the ground fades into
uniform vec3 u_sun_dir;
uniform vec3 u_sun_color;
uniform float u_sun_intensity;
uniform vec3 u_sky;            // hemisphere sky (up) color
uniform float u_ambient;
out vec4 frag_color;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float value_noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  float a = hash(i);
  float b = hash(i + vec2(1.0, 0.0));
  float c = hash(i + vec2(0.0, 1.0));
  float d = hash(i + vec2(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main() {
  // Two octaves of value noise (~1 m and ~5 m) for subtly mottled grass.
  float n = 0.65 * value_noise(v_world) + 0.35 * value_noise(v_world * 0.2);
  vec3 grass = mix(vec3(0.20, 0.32, 0.15), vec3(0.34, 0.46, 0.22), n);

  // Flat +Z surface lit by the scene environment (matches the mesh shader for
  // an up-facing normal): hemisphere sky ambient + one directional sun.
  vec3 lighting = (u_sky * u_ambient) +
                  (u_sun_color * (u_sun_intensity * max(normalize(u_sun_dir).z, 0.0)));
  vec3 color = grass * lighting;

  // Melt into the sky horizon with distance so the infinite plane has no hard
  // tiling edge; scaled by eye height so it adapts to zoom.
  float dist = distance(v_world, u_eye.xy);
  float fade = 1.0 - exp(-dist / (max(u_eye.z, 4.0) * 12.0));
  frag_color = vec4(mix(color, u_horizon, fade), 1.0);
}
)";

// Column-major 4x4 identity — the model matrix for non-instanced draws.
constexpr std::array<float, 16> kIdentity{
    1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};

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

std::uint32_t link_program(const char* vs_source, const char* fs_source) {
  const std::uint32_t vs = compile(gl::kVertexShader, vs_source);
  const std::uint32_t fs = compile(gl::kFragmentShader, fs_source);
  if (vs == 0 || fs == 0) {
    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    return 0;
  }
  const gl::GLuint program = gl::CreateProgram();
  gl::AttachShader(program, vs);
  gl::AttachShader(program, fs);
  gl::LinkProgram(program);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  gl::GLint status = 0;
  gl::GetProgramiv(program, gl::kLinkStatus, &status);
  if (status == 0) {
    std::string log(512, '\0');
    gl::GetProgramInfoLog(program, static_cast<gl::GLsizei>(log.size()), nullptr, log.data());
    spdlog::error("program link failed: {}", log.c_str());
    gl::DeleteProgram(program);
    return 0;
  }
  return program;
}

} // namespace

GLRenderer::~GLRenderer() {
  shutdown();
}

bool GLRenderer::init() {
  // Precondition: gl::load_functions(resolver) succeeded with the current
  // context — the windowing layer owns the resolver, not the renderer.
  program_ = link_program(kVertexShader, kFragmentShader);
  sky_program_ = link_program(kSkyVertexShader, kSkyFragmentShader);
  grid_program_ = link_program(kGridVertexShader, kGridFragmentShader);
  ground_program_ = link_program(kGroundVertexShader, kGroundFragmentShader);
  if (program_ == 0 || sky_program_ == 0 || grid_program_ == 0 || ground_program_ == 0) {
    return false;
  }
  u_view_ = gl::GetUniformLocation(program_, "u_view");
  u_projection_ = gl::GetUniformLocation(program_, "u_projection");
  u_model_ = gl::GetUniformLocation(program_, "u_model");
  u_use_instancing_ = gl::GetUniformLocation(program_, "u_use_instancing");
  u_color_ = gl::GetUniformLocation(program_, "u_color");
  u_highlight_ = gl::GetUniformLocation(program_, "u_highlight");
  u_accent_ = gl::GetUniformLocation(program_, "u_accent");
  u_lit_ = gl::GetUniformLocation(program_, "u_lit");
  u_has_texture_ = gl::GetUniformLocation(program_, "u_has_texture");
  u_base_color_ = gl::GetUniformLocation(program_, "u_base_color");
  u_uv_scale_ = gl::GetUniformLocation(program_, "u_uv_scale");
  u_tint_ = gl::GetUniformLocation(program_, "u_tint");
  u_has_normal_ = gl::GetUniformLocation(program_, "u_has_normal");
  u_normal_ = gl::GetUniformLocation(program_, "u_normal");
  u_normal_strength_ = gl::GetUniformLocation(program_, "u_normal_strength");
  u_has_roughness_ = gl::GetUniformLocation(program_, "u_has_roughness");
  u_roughness_map_ = gl::GetUniformLocation(program_, "u_roughness_map");
  u_roughness_ = gl::GetUniformLocation(program_, "u_roughness");
  u_camera_pos_ = gl::GetUniformLocation(program_, "u_camera_pos");
  u_sun_dir_ = gl::GetUniformLocation(program_, "u_sun_dir");
  u_sun_color_ = gl::GetUniformLocation(program_, "u_sun_color");
  u_sun_intensity_ = gl::GetUniformLocation(program_, "u_sun_intensity");
  u_sky_light_ = gl::GetUniformLocation(program_, "u_sky_light");
  u_ground_light_ = gl::GetUniformLocation(program_, "u_ground_light");
  u_ambient_ = gl::GetUniformLocation(program_, "u_ambient");
  u_sky_top_ = gl::GetUniformLocation(sky_program_, "u_sky_top");
  u_sky_horizon_ = gl::GetUniformLocation(sky_program_, "u_sky_horizon");
  u_grid_view_ = gl::GetUniformLocation(grid_program_, "u_view");
  u_grid_projection_ = gl::GetUniformLocation(grid_program_, "u_projection");
  u_grid_eye_ = gl::GetUniformLocation(grid_program_, "u_eye");
  u_grid_major_ = gl::GetUniformLocation(grid_program_, "u_major");
  u_grid_minor_ = gl::GetUniformLocation(grid_program_, "u_minor");
  u_grid_axis_x_ = gl::GetUniformLocation(grid_program_, "u_axis_x");
  u_grid_axis_y_ = gl::GetUniformLocation(grid_program_, "u_axis_y");
  u_ground_view_ = gl::GetUniformLocation(ground_program_, "u_view");
  u_ground_projection_ = gl::GetUniformLocation(ground_program_, "u_projection");
  u_ground_eye_ = gl::GetUniformLocation(ground_program_, "u_eye");
  u_ground_base_z_ = gl::GetUniformLocation(ground_program_, "u_base_z");
  u_ground_horizon_ = gl::GetUniformLocation(ground_program_, "u_horizon");
  u_ground_sun_dir_ = gl::GetUniformLocation(ground_program_, "u_sun_dir");
  u_ground_sun_color_ = gl::GetUniformLocation(ground_program_, "u_sun_color");
  u_ground_sun_intensity_ = gl::GetUniformLocation(ground_program_, "u_sun_intensity");
  u_ground_sky_ = gl::GetUniformLocation(ground_program_, "u_sky");
  u_ground_ambient_ = gl::GetUniformLocation(ground_program_, "u_ambient");
  gl::GenVertexArrays(1, &empty_vao_);
  ready_ = true;
  return true;
}

void GLRenderer::shutdown() {
  if (!ready_) {
    return;
  }
  clear_meshes();
  for (const auto& [id, tex] : textures_) {
    gl::DeleteTextures(1, &tex);
  }
  textures_.clear();
  gl::DeleteProgram(program_);
  gl::DeleteProgram(sky_program_);
  gl::DeleteProgram(grid_program_);
  gl::DeleteProgram(ground_program_);
  gl::DeleteVertexArrays(1, &empty_vao_);
  program_ = 0;
  sky_program_ = 0;
  grid_program_ = 0;
  ground_program_ = 0;
  empty_vao_ = 0;
  ready_ = false;
}

void GLRenderer::set_backdrop(const BackdropColors& colors) {
  backdrop_ = colors;
}

RenderMeshHandle GLRenderer::upload(const RenderMeshData& data) {
  if (!ready_ || data.positions.empty() || data.indices.empty()) {
    return {};
  }
  GpuMesh mesh;
  mesh.color = data.color;
  mesh.kind = data.kind;
  mesh.index_count = static_cast<std::int32_t>(data.indices.size());

  // Interleave position + normal + uv (one vertex format, stride 8). Lines get
  // zero normals; meshes without UVs (markings, lines) get (0,0) — harmless
  // because their Material carries no texture (u_has_texture stays 0).
  const std::size_t vertex_count = data.positions.size() / 3;
  std::vector<float> interleaved;
  interleaved.reserve(vertex_count * 8);
  const bool has_normals = data.normals.size() == data.positions.size();
  const bool has_uvs = data.uvs.size() == vertex_count * 2;
  for (std::size_t i = 0; i < vertex_count; ++i) {
    interleaved.push_back(data.positions[(i * 3) + 0]);
    interleaved.push_back(data.positions[(i * 3) + 1]);
    interleaved.push_back(data.positions[(i * 3) + 2]);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 0] : 0.0F);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 1] : 0.0F);
    interleaved.push_back(has_normals ? data.normals[(i * 3) + 2] : 1.0F);
    interleaved.push_back(has_uvs ? data.uvs[(i * 2) + 0] : 0.0F);
    interleaved.push_back(has_uvs ? data.uvs[(i * 2) + 1] : 0.0F);
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

  const auto stride = static_cast<gl::GLsizei>(8 * sizeof(float));
  gl::EnableVertexAttribArray(0);
  gl::VertexAttribPointer(0, 3, gl::kFloat, 0, stride, nullptr);
  gl::EnableVertexAttribArray(1);
  gl::VertexAttribPointer(
      1, 3, gl::kFloat, 0, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
  gl::EnableVertexAttribArray(2);
  gl::VertexAttribPointer(
      2, 2, gl::kFloat, 0, stride, reinterpret_cast<const void*>(6 * sizeof(float)));
  gl::BindVertexArray(0);

  const RenderMeshHandle handle{next_id_++};
  meshes_.emplace(handle.id, mesh);
  return handle;
}

void GLRenderer::destroy(GpuMesh& mesh) {
  gl::DeleteBuffers(1, &mesh.vbo);
  gl::DeleteBuffers(1, &mesh.ebo);
  if (mesh.instance_vbo != 0) {
    gl::DeleteBuffers(1, &mesh.instance_vbo);
  }
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

TextureHandle GLRenderer::upload(const TextureData& data) {
  const std::size_t expected =
      static_cast<std::size_t>(data.width) * static_cast<std::size_t>(data.height) * 4U;
  if (!ready_ || data.width <= 0 || data.height <= 0 || data.rgba.size() != expected) {
    return {};
  }
  gl::GLuint tex = 0;
  gl::GenTextures(1, &tex);
  gl::BindTexture(gl::kTexture2D, tex);
  gl::TexImage2D(gl::kTexture2D,
                 0,
                 static_cast<gl::GLint>(gl::kRgba8),
                 data.width,
                 data.height,
                 0,
                 gl::kRgba,
                 gl::kUnsignedByte,
                 data.rgba.data());
  gl::GenerateMipmap(gl::kTexture2D);
  const gl::GLenum wrap = data.wrap == TextureWrap::ClampToEdge ? gl::kClampToEdge : gl::kRepeat;
  gl::TexParameteri(gl::kTexture2D, gl::kTextureWrapS, static_cast<gl::GLint>(wrap));
  gl::TexParameteri(gl::kTexture2D, gl::kTextureWrapT, static_cast<gl::GLint>(wrap));
  gl::TexParameteri(
      gl::kTexture2D, gl::kTextureMinFilter, static_cast<gl::GLint>(gl::kLinearMipmapLinear));
  gl::TexParameteri(gl::kTexture2D, gl::kTextureMagFilter, static_cast<gl::GLint>(gl::kLinear));
  gl::BindTexture(gl::kTexture2D, 0);

  const TextureHandle handle{next_tex_id_++};
  textures_.emplace(handle.id, tex);
  return handle;
}

void GLRenderer::remove(TextureHandle handle) {
  const auto found = textures_.find(handle.id);
  if (found != textures_.end()) {
    gl::DeleteTextures(1, &found->second);
    textures_.erase(found);
  }
}

void GLRenderer::set_environment(const Environment& env) {
  environment_ = env;
}

void GLRenderer::set_ground(bool enabled, float base_z) {
  ground_enabled_ = enabled;
  ground_base_z_ = base_z;
}

void GLRenderer::draw_backdrop(const CameraMatrices& camera) {
  gl::BindVertexArray(empty_vao_);

  // Sky gradient: no depth interaction at all.
  gl::Disable(gl::kDepthTest);
  gl::UseProgram(sky_program_);
  gl::Uniform3f(u_sky_top_, backdrop_.sky_top[0], backdrop_.sky_top[1], backdrop_.sky_top[2]);
  gl::Uniform3f(
      u_sky_horizon_, backdrop_.sky_horizon[0], backdrop_.sky_horizon[1], backdrop_.sky_horizon[2]);
  gl::DrawArrays(gl::kTriangles, 0, 3);
  gl::Enable(gl::kDepthTest);

  // Ground grid: alpha-blended, no depth writes — meshes drawn afterwards
  // overwrite it wherever they cover the plane (grid sits at z = 0 too).
  gl::UseProgram(grid_program_);
  gl::UniformMatrix4fv(u_grid_view_, 1, 0, camera.view.data());
  gl::UniformMatrix4fv(u_grid_projection_, 1, 0, camera.projection.data());
  gl::Uniform3f(u_grid_eye_, camera.eye[0], camera.eye[1], camera.eye[2]);
  gl::Uniform4f(u_grid_major_,
                backdrop_.grid_major[0],
                backdrop_.grid_major[1],
                backdrop_.grid_major[2],
                backdrop_.grid_major[3]);
  gl::Uniform4f(u_grid_minor_,
                backdrop_.grid_minor[0],
                backdrop_.grid_minor[1],
                backdrop_.grid_minor[2],
                backdrop_.grid_minor[3]);
  gl::Uniform3f(u_grid_axis_x_, backdrop_.axis_x[0], backdrop_.axis_x[1], backdrop_.axis_x[2]);
  gl::Uniform3f(u_grid_axis_y_, backdrop_.axis_y[0], backdrop_.axis_y[1], backdrop_.axis_y[2]);
  gl::DepthMask(0);
  gl::Enable(gl::kBlend);
  gl::BlendFunc(gl::kSrcAlpha, gl::kOneMinusSrcAlpha);
  gl::DrawArrays(gl::kTriangleStrip, 0, 4);
  gl::Disable(gl::kBlend);
  gl::DepthMask(1);
  gl::BindVertexArray(0);
}

void GLRenderer::draw_ground(const CameraMatrices& camera) {
  if (!ground_enabled_) {
    return;
  }
  // Opaque, depth-tested AND depth-written (unlike the grid) so the road
  // network — drawn afterwards and sitting above ground_base_z_ — occludes it.
  gl::BindVertexArray(empty_vao_);
  gl::UseProgram(ground_program_);
  gl::UniformMatrix4fv(u_ground_view_, 1, 0, camera.view.data());
  gl::UniformMatrix4fv(u_ground_projection_, 1, 0, camera.projection.data());
  gl::Uniform3f(u_ground_eye_, camera.eye[0], camera.eye[1], camera.eye[2]);
  gl::Uniform1f(u_ground_base_z_, ground_base_z_);
  gl::Uniform3f(u_ground_horizon_,
                backdrop_.sky_horizon[0],
                backdrop_.sky_horizon[1],
                backdrop_.sky_horizon[2]);
  gl::Uniform3f(
      u_ground_sun_dir_, environment_.sun_dir[0], environment_.sun_dir[1], environment_.sun_dir[2]);
  gl::Uniform3f(u_ground_sun_color_,
                environment_.sun_color[0],
                environment_.sun_color[1],
                environment_.sun_color[2]);
  gl::Uniform1f(u_ground_sun_intensity_, environment_.sun_intensity);
  gl::Uniform3f(u_ground_sky_,
                environment_.sky_color[0],
                environment_.sky_color[1],
                environment_.sky_color[2]);
  gl::Uniform1f(u_ground_ambient_, environment_.ambient);
  gl::DrawArrays(gl::kTriangleStrip, 0, 4);
  gl::BindVertexArray(0);
}

void GLRenderer::render(const std::vector<DrawItem>& items,
                        const CameraMatrices& camera,
                        int width,
                        int height) {
  if (!ready_ || width <= 0 || height <= 0) {
    return;
  }
  gl::Viewport(0, 0, width, height);
  gl::ClearColor(
      backdrop_.sky_horizon[0], backdrop_.sky_horizon[1], backdrop_.sky_horizon[2], 1.0F);
  gl::Clear(gl::kColorBufferBit | gl::kDepthBufferBit);
  gl::Enable(gl::kDepthTest);
  gl::DepthFunc(gl::kLess);

  draw_backdrop(camera);
  draw_ground(camera);

  gl::UseProgram(program_);
  gl::UniformMatrix4fv(u_view_, 1, 0, camera.view.data());
  gl::UniformMatrix4fv(u_projection_, 1, 0, camera.projection.data());
  gl::Uniform3f(u_accent_, backdrop_.highlight[0], backdrop_.highlight[1], backdrop_.highlight[2]);
  gl::Uniform3f(
      u_sun_dir_, environment_.sun_dir[0], environment_.sun_dir[1], environment_.sun_dir[2]);
  gl::Uniform3f(u_sun_color_,
                environment_.sun_color[0],
                environment_.sun_color[1],
                environment_.sun_color[2]);
  gl::Uniform1f(u_sun_intensity_, environment_.sun_intensity);
  gl::Uniform3f(u_sky_light_,
                environment_.sky_color[0],
                environment_.sky_color[1],
                environment_.sky_color[2]);
  gl::Uniform3f(u_ground_light_,
                environment_.ground_color[0],
                environment_.ground_color[1],
                environment_.ground_color[2]);
  gl::Uniform1f(u_ambient_, environment_.ambient);
  // World-space eye for the specular half-vector (fed the same way sun_dir is).
  gl::Uniform3f(u_camera_pos_, camera.eye[0], camera.eye[1], camera.eye[2]);

  // Accent mix strength per feedback state: a subtle brighten on hover, a
  // stronger tint on selection (both toward the theme accent).
  const auto highlight_strength = [](HighlightState state) {
    switch (state) {
    case HighlightState::Selected:
      return 0.62F;
    case HighlightState::Hover:
      return 0.30F;
    case HighlightState::None:
      break;
    }
    return 0.0F;
  };

  gl::Uniform1i(u_base_color_, 0);    // sampler bound to texture unit 0
  gl::Uniform1i(u_normal_, 1);        // normal map on unit 1
  gl::Uniform1i(u_roughness_map_, 2); // roughness map on unit 2

  for (const DrawItem& item : items) {
    const auto found = meshes_.find(item.mesh.id);
    if (found == meshes_.end()) {
      continue;
    }
    // Non-const: the instanced path lazily creates mesh.instance_vbo on first use.
    GpuMesh& mesh = found->second;

    // Material resolution: sample the base-color texture when the material
    // carries one, else fall back to the mesh's flat color — a
    // default-constructed Material reproduces the pre-material output exactly.
    const Material& mat = item.material;
    const auto find_tex = [this](TextureHandle handle) -> std::uint32_t {
      if (!handle.valid()) {
        return 0;
      }
      const auto it = textures_.find(handle.id);
      return it == textures_.end() ? 0 : it->second;
    };
    const std::uint32_t base_tex = find_tex(mat.base_color);
    const std::uint32_t normal_tex = find_tex(mat.normal);
    const std::uint32_t roughness_tex = find_tex(mat.roughness);

    // Normal (unit 1) + roughness (unit 2) bind first; base color binds LAST on
    // unit 0 so the active unit is reset to 0 for any later fixed-unit-0 code.
    gl::Uniform1i(u_has_normal_, normal_tex != 0 ? 1 : 0);
    if (normal_tex != 0) {
      gl::ActiveTexture(gl::kTexture1);
      gl::BindTexture(gl::kTexture2D, normal_tex);
      gl::Uniform1f(u_normal_strength_, mat.normal_strength);
    }
    gl::Uniform1i(u_has_roughness_, roughness_tex != 0 ? 1 : 0);
    if (roughness_tex != 0) {
      gl::ActiveTexture(gl::kTexture2);
      gl::BindTexture(gl::kTexture2D, roughness_tex);
    }
    // Scalar roughness fallback/modulator: default 1.0 → zero specular, so an
    // untextured/default Material stays pixel-identical to the pre-PBR output.
    gl::Uniform1f(u_roughness_, mat.roughness_value);

    const bool has_tex = base_tex != 0;
    gl::Uniform1i(u_has_texture_, has_tex ? 1 : 0);
    // uv_scale drives all three samplers, so set it whenever any map is bound.
    gl::Uniform1f(u_uv_scale_, mat.uv_scale);
    gl::ActiveTexture(gl::kTexture0);
    if (has_tex) {
      gl::BindTexture(gl::kTexture2D, base_tex);
      gl::Uniform4f(u_tint_, mat.tint[0], mat.tint[1], mat.tint[2], mat.tint[3]);
    }
    gl::Uniform4f(u_color_, mesh.color[0], mesh.color[1], mesh.color[2], mesh.color[3]);
    // u_highlight and u_lit are `float` uniforms: they MUST be set with
    // Uniform1f. glUniform4f on a float uniform is GL_INVALID_OPERATION and
    // silently leaves the uniform at 0 (which is why the surface highlight
    // never rendered before).
    gl::Uniform1f(u_highlight_, highlight_strength(item.state));
    const bool lit = mesh.kind == PrimitiveKind::Triangles && !mat.unlit;
    gl::Uniform1f(u_lit_, lit ? 1.0F : 0.0F);
    gl::BindVertexArray(mesh.vao);

    const gl::GLenum primitive =
        mesh.kind == PrimitiveKind::Triangles ? gl::kTriangles : gl::kLines;
    if (item.instances.empty()) {
      // Non-instanced: the u_model path, bit-identical to before instancing.
      gl::Uniform1i(u_use_instancing_, 0);
      gl::UniformMatrix4fv(u_model_, 1, 0, kIdentity.data());
      gl::DrawElements(primitive, mesh.index_count, gl::kUnsignedInt, nullptr);
    } else {
      // Instanced fast path: one glDrawElementsInstanced for the whole span.
      // Per-instance mat4 columns feed attribs 3-6 (divisor 1). The instance
      // VBO is created lazily, then re-uploaded per draw (1k instances = 64 KB).
      static_assert(sizeof(InstanceData) == 64, "instance stride must be a column-major mat4");
      if (mesh.instance_vbo == 0) {
        gl::GenBuffers(1, &mesh.instance_vbo);
      }
      gl::BindBuffer(gl::kArrayBuffer, mesh.instance_vbo);
      // Fill the buffer BEFORE enabling attribs 3-6 so they never point at an
      // empty store.
      gl::BufferData(gl::kArrayBuffer,
                     static_cast<gl::GLsizeiptr>(item.instances.size() * sizeof(InstanceData)),
                     item.instances.data(),
                     gl::kDynamicDraw);
      const auto stride = static_cast<gl::GLsizei>(sizeof(InstanceData));
      for (gl::GLuint col = 0; col < 4; ++col) {
        const gl::GLuint loc = 3 + col;
        gl::EnableVertexAttribArray(loc);
        gl::VertexAttribPointer(
            loc,
            4,
            gl::kFloat,
            0,
            stride,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(col * 4 * sizeof(float))));
        gl::VertexAttribDivisor(loc, 1);
      }
      gl::Uniform1i(u_use_instancing_, 1);
      gl::DrawElementsInstanced(primitive,
                                mesh.index_count,
                                gl::kUnsignedInt,
                                nullptr,
                                static_cast<gl::GLsizei>(item.instances.size()));
    }
  }
  gl::BindVertexArray(0);
  gl::UseProgram(0);
}

} // namespace roadmaker::editor
