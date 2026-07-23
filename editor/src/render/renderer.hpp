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

// Renderer abstraction. NO OpenGL types or calls in this header — GL lives
// exclusively in editor/src/render/gl_*.  A future bgfx/WebGPU backend
// replaces GLRenderer without touching viewport or panel code.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace roadmaker::editor {

struct RenderMeshHandle {
  std::uint32_t id = 0;

  [[nodiscard]] bool valid() const { return id != 0; }
};

/// Opaque handle to an uploaded texture (mirrors RenderMeshHandle). id 0 =
/// "no texture"; a Material with an invalid base_color renders flat.
struct TextureHandle {
  std::uint32_t id = 0;

  [[nodiscard]] bool valid() const { return id != 0; }
};

/// How a texture samples outside [0,1]. Repeat is the M2 default (tiled road
/// surfaces); ClampToEdge is for atlas-like single images (sign faces) whose
/// baked margin must not wrap and bleed at the edges.
enum class TextureWrap {
  Repeat,
  ClampToEdge,
};

/// CPU-side texture upload, RGBA8 row-major (4 bytes/texel, width*height
/// texels). The render boundary stays GL-free: no sampler/format enum here,
/// the GL backend picks the internal format.
struct TextureData {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;
  /// Wrap mode. Default Repeat keeps existing (tiled surface) behaviour byte
  /// for byte; sign faces upload ClampToEdge.
  TextureWrap wrap = TextureWrap::Repeat;
};

/// A material references an optional base-color texture plus scalar factors.
/// Defaults reproduce the M2 flat look: no texture, white tint (the per-mesh
/// RenderMeshData::color remains the flat fallback when base_color is invalid,
/// so a default-constructed Material never changes existing output). Resolved
/// to a shader uniform set by the GL backend — no GL here.
struct Material {
  TextureHandle base_color;                          ///< invalid → use RenderMeshData::color
  TextureHandle normal;                              ///< invalid → geometric normal
  TextureHandle roughness;                           ///< invalid → roughness_value scalar
  std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F}; ///< multiplies the sample
  float uv_scale = 0.25F;                            ///< texels per meter: 0.25 = 4 m tile
  /// Scalar roughness fallback/modulator. DEFAULT 1.0 is fully rough: the sun
  /// specular lobe is scaled by (1 - roughness), so a default-constructed
  /// Material adds ZERO specular and reproduces the pre-PBR output exactly.
  float roughness_value = 1.0F;
  float normal_strength = 1.0F; ///< perturbation scale for the normal map
  bool unlit = false;           ///< markings/sky bypass lighting
};

/// Per-instance transform for prop instancing (many props share one mesh).
/// Column-major 4x4, kernel frame (Z-up, meters).
struct InstanceData {
  std::array<float, 16> model{1.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              1.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              1.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              0.0F,
                              1.0F};
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
  std::vector<float> uvs;       // uv pairs (u=s, v=t in meters); empty = untextured
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
  Material material{};                     ///< defaults reproduce the flat look
  std::span<const InstanceData> instances; ///< empty = one draw at identity
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

/// Scene lighting + sky description (all plain floats — theme/render boundary
/// stays Qt- and GL-free). Hemisphere ambient (sky/ground lerp by normal.z) +
/// one directional sun; no shadow maps (M3a decision 3). The DEFAULT values are
/// the daytime "Textured" preset (the new default render mode); `sober_lighting`
/// below reproduces the flat M2 look for the Sober toggle.
struct Environment {
  std::array<float, 3> sun_dir{0.35F, 0.25F, 0.90F}; ///< world, Z-up (to light)
  std::array<float, 3> sun_color{1.0F, 0.97F, 0.90F};
  std::array<float, 3> sky_color{0.5F, 0.7F, 1.0F};      ///< hemisphere up
  std::array<float, 3> ground_color{0.4F, 0.38F, 0.35F}; ///< hemisphere down
  float sun_intensity = 1.0F;
  float ambient = 0.35F;
  bool procedural_sky = true; ///< false → sampled HDRI (later render polish)
};

/// The daytime "Textured" render mode: hemisphere sky/ground + a warm sun.
[[nodiscard]] inline Environment textured_lighting() {
  return Environment{};
}

/// The "Sober" render mode — reproduces the M2 flat look exactly: a white,
/// normal-independent ambient of 0.35 plus a white directional term of
/// 0.65*lambert along the original hardcoded light direction, i.e. the old
/// `0.35 + 0.65*lambert` grey shading. Kept as the packaging/CI smoke path.
[[nodiscard]] inline Environment sober_lighting() {
  Environment env;
  env.sun_dir = {0.35F, 0.25F, 0.90F};
  env.sun_color = {1.0F, 1.0F, 1.0F};
  env.sky_color = {1.0F, 1.0F, 1.0F};    // sky == ground → normal-independent
  env.ground_color = {1.0F, 1.0F, 1.0F}; // ambient term, so no hemisphere tint
  env.sun_intensity = 0.65F;
  env.ambient = 0.35F;
  env.procedural_sky = false;
  return env;
}

class Renderer {
public:
  virtual ~Renderer() = default;

  virtual bool init() = 0;
  virtual void shutdown() = 0;

  virtual RenderMeshHandle upload(const RenderMeshData& data) = 0;
  virtual void remove(RenderMeshHandle handle) = 0;
  virtual void clear_meshes() = 0;

  /// Uploads an RGBA8 texture and returns an opaque handle; TextureData may be
  /// discarded after the call. Safe only after init().
  virtual TextureHandle upload(const TextureData& data) = 0;
  virtual void remove(TextureHandle handle) = 0;

  /// Sets the scene lighting/sky environment. Safe to call before init(); the
  /// frame after the call uses the new environment.
  virtual void set_environment(const Environment& env) = 0;

  /// Enables the procedural grass ground plane at world height `base_z` (meters,
  /// Z-up), drawn opaque with depth so the road network occludes it. Disabled =
  /// no ground (the Sober render mode). Safe to call before init(); it only
  /// stores state. `base_z` should sit just under the network so coplanar road
  /// surfaces don't z-fight (see scene_builder `ground_base_z`).
  virtual void set_ground(bool enabled, float base_z) = 0;

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
