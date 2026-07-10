#pragma once

// Renderer abstraction. NO OpenGL types or calls in this header — GL lives
// exclusively in editor/src/render/gl_*.  A future bgfx/WebGPU backend
// replaces GLRenderer without touching viewport or panel code.

#include <array>
#include <cstdint>
#include <vector>

namespace roadmaker::editor {

struct RenderMeshHandle {
  std::uint32_t id = 0;

  [[nodiscard]] bool valid() const { return id != 0; }
};

enum class PrimitiveKind {
  Triangles,
  Lines,
};

/// CPU-side mesh data ready for upload. Kernel frame (Z-up, meters),
/// float32 as this is a render boundary.
struct RenderMeshData {
  std::vector<float> positions; // xyz triplets
  std::vector<float> normals;   // xyz triplets (empty for Lines)
  std::vector<std::uint32_t> indices;
  std::array<float, 4> color{0.8F, 0.8F, 0.8F, 1.0F};
  PrimitiveKind kind = PrimitiveKind::Triangles;
};

struct DrawItem {
  RenderMeshHandle mesh;
  bool highlighted = false;
};

/// Right-handed, Z-up camera matrices (column-major 4x4).
struct CameraMatrices {
  std::array<float, 16> view{};
  std::array<float, 16> projection{};
};

class Renderer {
public:
  virtual ~Renderer() = default;

  virtual bool init() = 0;
  virtual void shutdown() = 0;

  virtual RenderMeshHandle upload(const RenderMeshData& data) = 0;
  virtual void remove(RenderMeshHandle handle) = 0;
  virtual void clear_meshes() = 0;

  /// Renders one frame into the currently bound framebuffer region.
  /// `width`/`height` are FRAMEBUFFER pixels (HiDPI-safe), not window units.
  virtual void render(const std::vector<DrawItem>& items,
                      const CameraMatrices& camera,
                      int width,
                      int height) = 0;
};

} // namespace roadmaker::editor
