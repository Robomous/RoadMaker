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

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "render/renderer.hpp"

namespace roadmaker::editor {

/// OpenGL 3.3 core implementation of Renderer. The ONLY file pair (with
/// gl_functions) that touches GL.
class GLRenderer final : public Renderer {
public:
  GLRenderer() = default;
  GLRenderer(const GLRenderer&) = delete;
  GLRenderer& operator=(const GLRenderer&) = delete;
  GLRenderer(GLRenderer&&) = delete;
  GLRenderer& operator=(GLRenderer&&) = delete;
  ~GLRenderer() override;

  bool init() override;
  void shutdown() override;

  RenderMeshHandle upload(const RenderMeshData& data) override;
  void remove(RenderMeshHandle handle) override;
  void clear_meshes() override;

  TextureHandle upload(const TextureData& data) override;
  void remove(TextureHandle handle) override;

  void set_environment(const Environment& env) override;
  void set_ground(bool enabled, float base_z) override;
  void set_backdrop(const BackdropColors& colors) override;

  void render(const std::vector<DrawItem>& items,
              const CameraMatrices& camera,
              int width,
              int height) override;

private:
  struct GpuMesh {
    std::uint32_t vao = 0;
    std::uint32_t vbo = 0;
    std::uint32_t ebo = 0;
    // Per-instance transform buffer (attribs 3-6), created lazily on the first
    // instanced draw of this mesh; 0 = never instanced. Deleted in destroy().
    std::uint32_t instance_vbo = 0;
    std::int32_t index_count = 0;
    std::array<float, 4> color{};
    PrimitiveKind kind = PrimitiveKind::Triangles;
  };

  void destroy(GpuMesh& mesh);
  void draw_backdrop(const CameraMatrices& camera);
  void draw_ground(const CameraMatrices& camera);

  std::unordered_map<std::uint32_t, GpuMesh> meshes_;
  std::uint32_t next_id_ = 1;
  // GL texture names keyed by TextureHandle::id (id space disjoint from meshes).
  std::unordered_map<std::uint32_t, std::uint32_t> textures_;
  std::uint32_t next_tex_id_ = 1;
  Environment environment_{};
  std::uint32_t program_ = 0;
  std::int32_t u_view_ = -1;
  std::int32_t u_projection_ = -1;
  std::int32_t u_model_ = -1;
  std::int32_t u_use_instancing_ = -1;
  std::int32_t u_color_ = -1;
  std::int32_t u_highlight_ = -1;
  std::int32_t u_accent_ = -1;
  std::int32_t u_lit_ = -1;
  std::int32_t u_has_texture_ = -1;
  std::int32_t u_base_color_ = -1;
  std::int32_t u_uv_scale_ = -1;
  std::int32_t u_tint_ = -1;
  // PBR-lite material uniforms (normal + roughness maps, per-draw specular).
  std::int32_t u_has_normal_ = -1;
  std::int32_t u_normal_ = -1;
  std::int32_t u_normal_strength_ = -1;
  std::int32_t u_has_roughness_ = -1;
  std::int32_t u_roughness_map_ = -1;
  std::int32_t u_roughness_ = -1;
  std::int32_t u_camera_pos_ = -1;
  // Environment lighting uniforms (driven by set_environment).
  std::int32_t u_sun_dir_ = -1;
  std::int32_t u_sun_color_ = -1;
  std::int32_t u_sun_intensity_ = -1;
  std::int32_t u_sky_light_ = -1;
  std::int32_t u_ground_light_ = -1;
  std::int32_t u_ambient_ = -1;

  // Backdrop passes (sky gradient + ground grid), both attributeless draws
  // through one shared empty VAO (core profile requires a bound VAO).
  BackdropColors backdrop_{};
  std::uint32_t empty_vao_ = 0;
  std::uint32_t sky_program_ = 0;
  std::int32_t u_sky_top_ = -1;
  std::int32_t u_sky_horizon_ = -1;
  std::uint32_t grid_program_ = 0;
  std::int32_t u_grid_view_ = -1;
  std::int32_t u_grid_projection_ = -1;
  std::int32_t u_grid_eye_ = -1;
  std::int32_t u_grid_major_ = -1;
  std::int32_t u_grid_minor_ = -1;
  std::int32_t u_grid_axis_x_ = -1;
  std::int32_t u_grid_axis_y_ = -1;

  // Procedural grass ground plane (textured mode only), at ground_base_z_.
  bool ground_enabled_ = false;
  float ground_base_z_ = 0.0F;
  std::uint32_t ground_program_ = 0;
  std::int32_t u_ground_view_ = -1;
  std::int32_t u_ground_projection_ = -1;
  std::int32_t u_ground_eye_ = -1;
  std::int32_t u_ground_base_z_ = -1;
  std::int32_t u_ground_horizon_ = -1;
  std::int32_t u_ground_sun_dir_ = -1;
  std::int32_t u_ground_sun_color_ = -1;
  std::int32_t u_ground_sun_intensity_ = -1;
  std::int32_t u_ground_sky_ = -1;
  std::int32_t u_ground_ambient_ = -1;

  bool ready_ = false;
};

} // namespace roadmaker::editor
