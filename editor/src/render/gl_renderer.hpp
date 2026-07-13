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
    std::int32_t index_count = 0;
    std::array<float, 4> color{};
    PrimitiveKind kind = PrimitiveKind::Triangles;
  };

  void destroy(GpuMesh& mesh);
  void draw_backdrop(const CameraMatrices& camera);

  std::unordered_map<std::uint32_t, GpuMesh> meshes_;
  std::uint32_t next_id_ = 1;
  std::uint32_t program_ = 0;
  std::int32_t u_view_ = -1;
  std::int32_t u_projection_ = -1;
  std::int32_t u_color_ = -1;
  std::int32_t u_highlight_ = -1;
  std::int32_t u_accent_ = -1;
  std::int32_t u_lit_ = -1;

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

  bool ready_ = false;
};

} // namespace roadmaker::editor
