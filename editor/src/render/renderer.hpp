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

/// Per-item feedback state. Drives the accent emphasis in the mesh shader:
/// None renders the base color, Hover a subtle accent brighten, Selected a
/// stronger accent tint. The accent color itself is a theme token handed to
/// the renderer via BackdropColors::highlight (render/ stays Qt-free).
enum class HighlightState {
  None,
  Hover,
  Selected,
};

struct DrawItem {
  RenderMeshHandle mesh;
  HighlightState state = HighlightState::None;
};

/// Right-handed, Z-up camera matrices (column-major 4x4).
struct CameraMatrices {
  std::array<float, 16> view{};
  std::array<float, 16> projection{};
  std::array<float, 3> eye{}; // world-space camera position (backdrop fade)
};

/// Viewport backdrop colors — sky gradient, ground grid, origin axes. Plain
/// floats because this is the theme→renderer boundary (render/ stays
/// Qt-free); values come from the active Theme (editor/src/theme/).
struct BackdropColors {
  std::array<float, 3> sky_top{0.137F, 0.153F, 0.180F};
  std::array<float, 3> sky_horizon{0.227F, 0.255F, 0.286F};
  std::array<float, 4> grid_major{0.239F, 0.263F, 0.294F, 0.85F};
  std::array<float, 4> grid_minor{0.149F, 0.169F, 0.192F, 0.55F};
  std::array<float, 3> axis_x{0.75F, 0.35F, 0.32F};
  std::array<float, 3> axis_y{0.35F, 0.65F, 0.36F};
  /// Selection/hover emphasis color (the theme `accent` token). Lives here
  /// because BackdropColors is the single theme→renderer float channel, wired
  /// to re-push on theme changes; the mesh shader mixes toward it per
  /// DrawItem::state.
  std::array<float, 3> highlight{0.961F, 0.651F, 0.137F};
};

class Renderer {
public:
  virtual ~Renderer() = default;

  virtual bool init() = 0;
  virtual void shutdown() = 0;

  virtual RenderMeshHandle upload(const RenderMeshData& data) = 0;
  virtual void remove(RenderMeshHandle handle) = 0;
  virtual void clear_meshes() = 0;

  /// Sets the sky/grid/axes colors. Safe to call before init(); the frame
  /// after the call uses the new colors.
  virtual void set_backdrop(const BackdropColors& colors) = 0;

  /// Renders one frame into the currently bound framebuffer region.
  /// `width`/`height` are FRAMEBUFFER pixels (HiDPI-safe), not window units.
  virtual void render(const std::vector<DrawItem>& items,
                      const CameraMatrices& camera,
                      int width,
                      int height) = 0;
};

} // namespace roadmaker::editor
