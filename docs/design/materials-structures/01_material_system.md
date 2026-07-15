# 01 — Material system v2 (PBR-lite + assignable library)

*How surfaces stop being tinted lane types and start being materials.*

## 1. What PBR-lite is, exactly

**Albedo + normal + roughness, sampled in the existing GL 3.3 lighting path.**
That is the whole definition, and it is a boundary rather than a starting point
(overview decision 4; the seed's top risk is this list growing).

Explicitly **not** in PBR-lite:

- No image-based lighting / environment probes
- No shadow maps
- No metallic workflow (every surface this milestone ships is a dielectric —
  asphalt, concrete, grass, painted steel)
- No parallax/displacement, no ambient occlusion maps, no emissive

The lighting model stays what M3a shipped: one directional sun + hemisphere
ambient. Roughness modulates a specular term; normal maps perturb the shading
normal. Nothing else changes.

### Why roughness matters more than it looks

Without a specular response, roughness has nothing to modulate and normal maps
are nearly invisible under pure Lambert. So WS-1 adds a **Blinn-Phong-style
specular lobe** driven by roughness — not because the milestone wants shininess,
but because it is what makes wet-looking new asphalt read differently from worn
asphalt. This is the *minimum* addition that makes GS-4's "two visibly distinct
asphalts" achievable, and it is in scope; anything past it is not.

## 2. Material definitions live in the manifest

`assets/library/manifest.json` gains a `materials[]` block alongside `items[]`.
Definitions are project assets, not scene data — a `.xodr` never carries a
texture path.

```json
{
  "manifest_version": 2,
  "materials": [
    {
      "id": "rm:asphalt_new",
      "label": "Asphalt (new)",
      "category": "Materials",
      "thumbnail": "assets/library/thumbnails/mat_asphalt_new.png",
      "maps": {
        "albedo": "assets/textures/asphalt/asphalt_new_diff_512.jpg",
        "normal": "assets/textures/asphalt/asphalt_new_nor_512.jpg",
        "roughness": "assets/textures/asphalt/asphalt_new_rough_512.jpg"
      },
      "uv_scale": 0.25,
      "params": { "roughness": 0.72, "normal_strength": 1.0, "tint": [1, 1, 1, 1] }
    }
  ]
}
```

- **`id` is the contract.** It is what a `.xodr` stores and what the renderer
  looks up. Namespaced `rm:` so an id is recognisable as ours in a foreign file
  and cannot collide with another tool's `surface` codes.
- **Any map may be absent.** Missing normal/roughness → the renderer falls back
  to flat/default values; missing albedo → falls back to the mesh's flat colour.
  This is what keeps old scenes rendering unchanged.
- **`params` are scalar fallbacks and modifiers**, applied whether or not the
  corresponding map exists.

**Schema versioning.** `manifest_version` goes 1 → 2. The existing loader
(`library_manifest.cpp`) already parses an unknown version best-effort with a
warning rather than crashing, and degrades unknown item kinds to `Unknown` —
so a v2 manifest in an older build shows the material entries as inert rows
instead of breaking. That forward-compat design is already there; WS-1 must not
regress it. A v1 manifest in a new build parses fine (no `materials[]` → no
materials, flat colours everywhere).

**Note the lock-in** (M3a's risk 4, still live): the M4 Asset Library Browser is
designed to subsume this panel and serve "props and materials in map mode". The
`materials[]` schema committed here is what M4 inherits.

## 3. Persistence — the split (DECIDED: no sidecar)

**The `.xodr` carries the assignment; the manifest carries the definition.**

The seed flagged this as an open question ("OpenDRIVE `<material>` records vs.
editor-side metadata — flag, don't decide"). It is decided here, because the
local spec text settles it and stalling blocks WS-2. Both halves of the decision
are grounded in normative text, not preference:

| What | Carrier | Standing |
|---|---|---|
| Lane surface material | `<lane><material sOffset surface="rm:id" friction roughness>` | **Standard** — §11.8.2, `t_road_lanes_laneSection_lr_lane_material`. `surface` is *"Surface material code, depending on application"* — an application-defined string, which is exactly a material id. |
| Road-mark material | `<roadMark material="rm:id">` | **Standard** — §11 roadMark attributes: *"Identifiers to be defined by the user, use `standard` as default value."* |
| Centre-lane surface | `<userData>` on the lane | The standard **forbids** the obvious carrier: `asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material` — "The center lane shall have no material elements." |
| Junction-floor surface | `<userData>` on the `<junction>` | No lane-material record exists for a junction surface; the floor is RoadMaker-generated geometry. |
| Bridge/structure surface | `<userData>` on the `<bridge>` object | Same — the solids are generated, not lane geometry. |
| Texture maps, tiling, PBR params, variants | **library manifest** | Project asset data. Never per-scene: two projects sharing a scene should not disagree about what `rm:asphalt_new` looks like. |

### Why `<userData>` and not a sidecar file

The obvious alternative — and the one considered first — is a clearly-versioned
editor sidecar next to the `.xodr`, holding whatever the standard cannot express.
Two pieces of evidence point the other way, and they agree:

1. **The standard offers the hook, and names our exact use case.** OpenDRIVE
   §7.2 (`<userData>`, `t_userData`): *"Additional or ancillary data contains
   data that are not yet described in ASAM OpenDRIVE, or data that is needed by
   an application for a specific reason, **for example different road
   textures**. It should be described near the element it refers to."* An
   application storing per-surface texture assignments is the textbook case the
   chapter was written for. Reaching past a sanctioned extension point for a
   bespoke file would be the unusual choice, not the conservative one.
2. **M2's frozen rule says the `.xodr` is the project file.** From
   `docs/design/m3a/05_editor_and_docs.md:80`, the autosave design: the recovery
   file "is a valid `.xodr`, **not a bespoke format** (the
   `.xodr`-is-the-project-file rule from M2)". A material sidecar reintroduces
   exactly what that rule exists to prevent.

And a practical failure mode decides it even if the above were a wash: **a
sidecar splits silently**. Email someone `scene.xodr`, or commit it without its
companion, and every material assignment vanishes with no error — the file still
parses, it just renders wrong. `<userData>` travels with the data it describes,
degrades to "unknown extension" in foreign tools, and round-trips through
RoadMaker's existing `RawXml` passthrough even in builds that predate this
milestone.

**The reversible part** is the `code` namespace, not the principle. If the
maintainer prefers different codes, that is a find-and-replace.

### `<userData>` encoding

One element per assignment, `code` namespaced, `value` holding the id:

```xml
<junction id="1" name="urban_x">
  <!-- non-standard surface; ASAM §7.2 sanctions userData for exactly this -->
  <userData code="rm:material.junction_floor" value="rm:asphalt_new"/>
</junction>
```

`code` is `rm:material.<surface-role>`; `value` is the material id. Flat
key/value, no nested JSON — a foreign tool reading it gets something legible,
and we avoid inventing a serialization format inside an attribute.

### Round-trip claim

Write → parse → write stays byte-identical, and a scene authored by another tool
that already carries `<material>` or `<userData>` keeps them verbatim (§4).

## 4. Kernel: promoting `<material>` out of the Preserved tier

**Today `<material>` is not modeled.** `core/include/roadmaker/xodr/raw_xml.hpp`
names it explicitly as an unmodeled child that the parser captures as a
self-contained XML fragment and the writer re-emits verbatim — the "Preserved"
tier from `docs/design/m3a/01` §5, the mechanism behind "the parser never
silently drops input".

WS-1 promotes it to the **Modeled** tier:

```cpp
/// <material> on a lane — ASAM OpenDRIVE 1.9.0 §11.8.2 (1.8.1 §11.7.2; the
/// element is identical, only the chapter number moved).
struct LaneMaterial {
  double s_offset = 0.0;
  std::string surface;        ///< application-defined code; RoadMaker writes "rm:<id>"
  std::optional<double> friction;
  std::optional<double> roughness;
  RawXml raw;                 ///< attributes we do not model, preserved verbatim
};
```

**This is risk 3, and the mitigation is structural:** the promotion claims
exactly the four attributes the standard defines and *keeps `RawXml` on the
struct* for everything else, so a foreign file's extra attributes survive. The
gate is a fixture — a hand-authored lane carrying a `<material>` with an
unmodeled attribute — that round-trips byte-identically. Fixture goes in the
same PR as the promotion, not after.

Validation, with rule ids cited in the diagnostic (per the repo's standing
requirement):

- `asam.net:xodr:1.4.0:road.lane.material.center_lane_no_material` — error if a
  centre lane (id 0) carries a `<material>`. This is also why centre-lane
  material assignment routes through `<userData>` (§3).
- `asam.net:xodr:1.4.0:road.lane.material.elem_asc_order` — the writer emits in
  ascending `sOffset`; the validator errors on a parsed file that is not.

`friction`/`roughness` are written faithfully and **not simulated** — RoadMaker
has no tyre model, and pretending otherwise would be worse than silence
(overview non-goals).

## 5. Renderer changes

`Material` (`editor/src/render/renderer.hpp:42`) today is
`{base_color, tint, uv_scale, unlit}`. It grows:

```cpp
struct Material {
  TextureHandle base_color;   ///< invalid -> RenderMeshData::color
  TextureHandle normal;       ///< invalid -> geometric normal
  TextureHandle roughness;    ///< invalid -> params.roughness scalar
  std::array<float, 4> tint{1, 1, 1, 1};
  float uv_scale = 0.25F;
  float roughness = 0.8F;     ///< scalar fallback / multiplier
  float normal_strength = 1.0F;
  bool unlit = false;
};
```

- **Texture units.** Only unit 0 is bound today, once before the draw loop.
  Normal → unit 1, roughness → unit 2, bound per draw item.
- **Tangents — decided (risk 1).** The vertex format is a fixed interleaved
  stride-8 (position/normal/uv) with no tangent, and normal mapping needs a
  basis. **Derive it per-fragment** from screen-space derivatives (`dFdx`/`dFdy`
  of world position and uv) rather than adding a vertex attribute. Rationale:
  every surface this milestone maps is planar-UV'd road/deck geometry where the
  derived basis is stable; a vertex-format change would touch the mesh path,
  the exporters, and every existing buffer for no visible gain. If a later case
  needs true tangents, adding the attribute is a contained change.
- **Fallback is the compatibility guarantee.** Every new sample is behind an
  `if (has_map)`; a material with no maps renders **exactly** as it does today.
  This is what makes "old scenes unaffected" testable rather than aspirational,
  and it is WS-1's gate.
- **Sober mode is untouched.** It bypasses materials already.

### Where assignment lands

`viewport_widget.cpp:135` `material_for(SurfaceKind)` currently switches a
four-value enum onto two hardcoded Qt-resource textures
(`:/textures/asphalt.jpg`). WS-2 replaces this with a lookup:
**surface → assigned material id → manifest definition → `Material`**, falling
back to the `SurfaceKind` default when a surface has no assignment. `DrawItem`
already carries a per-item `material`, so there is a slot to land in — no
draw-loop restructuring.

Bundled textures move from Qt resources to manifest-referenced files on disk, so
materials are data rather than a `.qrc` entry.

## 6. Assignment UX

Two paths to the same command, per the seed:

1. **Drag onto a surface** — drag a material from the Library onto a lane,
   junction floor, or structure in the viewport. Reuses the existing
   `resolve_library_drop` → `pick()` path that #179 fixed so the ghost lands
   where the cursor is; the drop target is a *surface*, not a position.
2. **Properties dropdown** — with a lane selected, a Material combo lists the
   library's materials, current assignment shown.

Both go through one `edit::assign_material(network, target, material_id)`
command — undoable, capturing values not pointers, per the M2 invariants.
Multi-select assign is a single command (one undo step), matching the
drag-session rule.

**Variants** are ordinary materials with sibling ids (`rm:asphalt_new`,
`rm:asphalt_worn`). No variant mechanism in the data model — a variant is a
naming convention and a thumbnail, which is enough for GS-4 and avoids inventing
a taxonomy nobody asked for.

## 7. Tests

- **Manifest**: v2 parse; v1 parses (no materials); unknown version warns not
  crashes; missing map keys tolerated; a material referencing a nonexistent file
  is a diagnostic, not a crash.
- **Kernel**: `<material>` round-trip byte-identical **including unmodeled
  attributes** (risk 3's fixture); centre-lane violation cites
  `center_lane_no_material`; out-of-order elements cite `elem_asc_order`;
  `<userData>` assignment survives write → parse → write.
- **Assignment**: `assign_material` apply → revert leaves `write_xodr()`
  byte-identical (the M2 command invariant); failed apply leaves the network
  untouched.
- **Render**: screenshot CI before/after of one road, flat vs PBR-lite. The
  **GS-1 baseline must not change more than trivially** — if the grass/asphalt
  upgrade moves it, refresh the baseline in the same PR with a note saying why
  (the golden-scenes README's rule).
- **UV/tiling**: fixtures pinning `uv_scale` → texels-per-metre so a tiling
  regression is caught by a number, not an eyeball.
