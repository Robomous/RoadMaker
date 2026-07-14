# M3a render — textured default, terrain, sky/lighting, instanced props

Goal: the viewport reads as a daytime road scene by default, without leaving the
thin-renderer posture. Every capability stays behind the `Renderer` interface
(`editor/src/render/renderer.hpp`); GL lives only in `editor/src/render/gl_*`
and the `ViewportWidget`; **sober mode remains** as a toggle and as the
packaging smoke path (`00` risk 2). No shadow maps (decision 3).

Baseline: M2's `Renderer` uploads `RenderMeshData` (positions, normals, indices,
one flat `color`, `PrimitiveKind`) and draws `DrawItem`s with a camera. There is
no material/texture, no instancing, no lighting model beyond flat shading. M3a
extends the interface minimally and additively.

## 1. Renderer interface extensions (still GL-free)

Additive to `renderer.hpp` — no GL types cross the boundary; textures and
instances are described as CPU-side data + opaque handles, exactly like
`RenderMeshHandle`:

```cpp
struct TextureHandle { std::uint32_t id = 0; bool valid() const { return id; } };

/// CPU-side texture upload (RGBA8, row-major). Kernel/editor stays GL-free.
struct TextureData { int width = 0, height = 0; std::vector<std::uint8_t> rgba; };

/// A material references a base-color texture (optional) + scalar factors.
/// Resolved to a shader uniform set by the GL backend; no GL here.
struct Material {
  TextureHandle base_color;          // invalid → use flat `color`
  std::array<float,4> color{0.8F,0.8F,0.8F,1.0F};
  float metallic = 0.0F, roughness = 0.9F;
  bool  unlit = false;               // markings/sky use unlit
};

/// Per-instance transform for prop instancing (props share one mesh).
struct InstanceData { std::array<float,16> model{}; };   // column-major, Z-up

struct DrawItem {                    // extended
  RenderMeshHandle mesh;
  Material material;                 // NEW (defaults reproduce M2 flat look)
  bool highlighted = false;
  std::span<const InstanceData> instances;  // NEW; empty = single draw
};

class Renderer {                     // additive methods
  virtual TextureHandle upload(const TextureData&) = 0;
  virtual void remove(TextureHandle) = 0;
  // existing upload/remove/clear/render unchanged; render() now honors
  // material + instances on each DrawItem, plus a scene environment:
  virtual void set_environment(const Environment&) = 0;   // sky + lights (§2)
};
```

`Environment` carries the hemisphere + directional lighting parameters and the
sky description — all plain floats/colors. A future bgfx/WebGPU backend
implements the same interface; the viewport and panels never change.

## 2. Sky and lighting pass (no shadows)

```cpp
struct Environment {
  std::array<float,3> sun_dir{0.3F,0.4F,-0.85F};   // Z-up world
  std::array<float,3> sun_color{1.0F,0.97F,0.9F};
  std::array<float,3> sky_color{0.5F,0.7F,1.0F};   // hemisphere up
  std::array<float,3> ground_color{0.4F,0.38F,0.35F}; // hemisphere down
  float sun_intensity = 1.0F, ambient = 0.3F;
  bool  procedural_sky = true;       // false → sampled HDRI (03 §1)
};
```

- **Lighting model:** hemisphere ambient (sky/ground lerp by normal.z) +
  one directional sun, evaluated in the fragment shader. Physically plausible,
  cheap, deterministic. **No shadow maps** — decision 3; shadows are M4+.
- **Sky:** a procedural gradient (horizon→zenith) drawn as a fullscreen
  background pass, or a sampled CC0 HDRI if procedural reads poorly (`03` §1).
  The sky is `unlit`.
- Sober mode = `Environment` with flat ambient and no sky (current M2 look),
  selected by the render-mode toggle (§5).

## 3. Prop instancing

GS-1 places ~20 vegetation props plus poles/signal heads. Each library model
uploads **once**; placed instances draw via `InstanceData` transforms:

- The scene builder groups placed objects by library `key`
  ([`03`](03_assets.md) §4), uploads the `.glb` mesh once per key, and emits one
  `DrawItem` with an `instances` span of per-object model matrices.
- The model matrix comes from the object's world placement: road `s`/`t`/
  `zOffset` → world position via the reference line + elevation, plus `hdg`/
  `pitch`/`roll` (or `perpToRoad`). This resolution lives in the editor scene
  builder, not the kernel (kernel stays render-free); it reuses the kernel's
  `s/t`→world evaluation exposed for the mesh path.
- Objects without a resolvable model (foreign/unmodeled) draw a **placeholder**
  box from the bounding volume — never nothing (visible = debuggable).
- The GL backend uses a single instanced draw call per key
  (`glDrawElementsInstanced`); the interface hides this behind the `instances`
  span so non-GL backends are free to batch differently.

## 4. Textures, terrain, and dirty re-upload

- **Textured roadway/sidewalk:** per-lane material selected by `LaneType`
  (asphalt for driving, concrete for sidewalk) with planar UVs generated in the
  mesh builder (s along the road, t across) — extends the optional textured mode
  scoped in [m2/05](../m2/05_assets.md), now wired to a `Material` per submesh.
- **Terrain skirt + procedural ground:** a ground plane / skirt mesh generated
  around the network's bounding footprint (a border ring extruded down to a base
  level so the network doesn't float), textured grass or a procedural ground
  shader. Generated in the editor render layer from the network bounds — it is
  scenery, not road-network data, so it is **not** a kernel mesh and never
  exports to `.xodr`.
- **Incremental re-upload:** M2's dirty-road re-mesh already re-uploads only
  changed meshes. M3a adds the object dirty set
  ([`01`](01_kernel_objects_signals.md) §2.4 — implemented in phase 2/#69,
  consumed here): moving a prop re-uploads only its
  instance transform, not the road surface. Textures upload once and are keyed
  by material.

## 5. Render-mode toggle & default

- Two modes: **Textured** (new default — materials, sky, lighting, props) and
  **Sober** (flat M2 look — solid colors, no sky, no textures). A
  `View → Rendering` toggle + a persisted setting; sober stays the CI/packaging
  smoke path so a GL-limited environment still renders.
- The toggle flips `Environment` and material resolution; geometry is identical,
  so switching is instant and needs no re-mesh.

## 6. Test plan

- **Headless frame test:** with `QT_QPA_PLATFORM=offscreen`, render GS-1 to an
  offscreen framebuffer in both modes; assert non-empty, deterministic pixel
  hash for a fixed camera (the golden-screenshot path).
- **Instancing:** N placed props of one key produce one `DrawItem` with N
  `InstanceData`; moving one updates only that transform.
- **Interface purity:** a compile-time check (existing test) that `renderer.hpp`
  pulls in no GL/Qt headers; the new texture/material/instance types are POD.
- **Sober parity:** sober mode still renders the M2 golden network identically
  (guards the packaging path).
- Shadows explicitly absent — a test asserting the lighting path allocates no
  shadow resources documents decision 3.

## 7. As-built — WS-A part 1 (interface + UVs + plumbing)

The additive interface (§1) landed adapted to the M2 renderer as it actually
shipped, with these deviations recorded here (design intent unchanged):

- **Flat-color fallback stays on `RenderMeshData::color`, not `Material`.** M2
  carries one flat color per uploaded mesh (not per `DrawItem`), and the M3a
  revamp added `HighlightState` on `DrawItem`. `Material` was added *alongside*
  these: an invalid `Material::base_color` means "use the mesh's flat color",
  so a default-constructed `Material` reproduces the pre-material output
  byte-for-byte. `Material::tint` therefore defaults to **white** (multiplies
  the texture sample), not the doc's `0.8` grey (which assumed `Material`
  owned the flat color).
- **UVs live on the kernel mesh in meters.** `SubMesh`/`RoadMesh` gained a
  `uvs` vector (u = s, v = t, meters); tiling is applied in the shader via
  `Material::uv_scale` (default `0.25` = 4 m tile) rather than baking the tile
  size into the coordinates — so a material can retile without a re-mesh. Empty
  `uvs` = untextured (markings, junction floors until A2/D2 assign them).
- **Instancing is a per-instance uniform loop for now.** `DrawItem::instances`
  is honoured by drawing once per `InstanceData` with a `u_model` uniform; the
  `glDrawElementsInstanced` fast path lands with prop instancing in WS-C. The
  interface (an `instances` span) is final; only the backend is provisional.
- **`set_environment` stores but does not yet light.** The `Environment` is
  captured; the hemisphere + directional shader that consumes it is the D1
  (textured-mode) PR, keeping WS-A part 1 a no-visual-change plumbing step.

Tests that landed with it: `Mesh.RoadGridCarriesContinuousPlanarUVs` (kernel UV
mapping + continuity), `ToRenderData.NarrowsUVsWhenProvided`,
`BuildScene.RoadPatchesInheritSharedGridUVs`, and
`Material.DefaultsAreFlatAndUninstanced` (the flat-look guarantee). The
offscreen frame/parity and instancing-behaviour tests (§6) land with D1/WS-C
when there is a visible surface to assert.

## 8. As-built — WS-D part 1 (lighting + render-mode toggle)

The §2 lighting pass and §5 toggle landed together:

- **`Environment` now drives the mesh fragment shader** (hemisphere ambient by
  `normal.z` + one directional sun), replacing the hardcoded
  `0.35 + 0.65*lambert`. Two presets live next to the struct in `renderer.hpp`:
  `textured_lighting()` (the daytime default, == `Environment{}`) and
  `sober_lighting()`, tuned so the shader reduces to the **exact** old grey
  shading (sky == ground == white, ambient 0.35, white sun at 0.65 along the
  original direction) — the sober-parity guarantee is a unit test, not just a
  pixel review.
- **The toggle is `View → Textured Rendering`** (checkable, persisted as
  `view/textured_rendering`, default on). It calls
  `ViewportWidget::set_textured_rendering`, which swaps the `Environment` and
  dims the reference-grid alpha in textured mode — no re-mesh, per §5. The
  screenshot/packaging path keeps rendering (sober stays available for
  GL-limited smoke).
- Tests: `Lighting.SoberPresetReproducesFlatM2Shading`,
  `Lighting.TexturedPresetIsTheDaytimeDefault`. The rendered look is captured by
  the `editor-visual-artifacts` CI job (Linux + xvfb); local macOS offscreen has
  no GL context, so pixel review happens on CI artifacts.

## 9. As-built — WS-D part 2 (procedural ground)

The §4 "terrain skirt + procedural ground" landed as a **procedural grass
ground plane** (grass stays procedural-first per `03_assets.md` §1):

- `Renderer::set_ground(enabled, base_z)` + a new GL pass draw a large
  camera-following quad at `base_z`, opaque with depth **write** (unlike the
  reference grid) so the road network occludes it. Grass colour is value-noise
  mottled and lit by the scene `Environment`, fading to the sky horizon with
  distance. Enabled in Textured mode only; Sober keeps the grid.
- `base_z` = `scene_builder::ground_base_z(SceneBounds)` = network floor − 5 cm,
  so coplanar road surfaces don't z-fight (unit-tested: `GroundBaseZ.*`).
- **Deviation from §4:** it is a flat plane at the network floor, not a
  bounds-fitted, elevation-following skirt — that is deferred to the terrain
  ADR-0006 / #83 (PROPOSED). GS-1 is flat, so the plane is exact for it. The
  per-material texture-vs-procedural decisions live in
  [`textured_render.md`](textured_render.md).
</content>
