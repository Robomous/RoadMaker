# M3a seed — visual & standards completeness

*Scope sketch for M3a. A seed, not a design: the full design docs are
written when M3a's own planning task runs, after M2 ships.*

- **Theme:** authored scenes look and validate like real road scenes.
- **Golden scene:** [GS-1 "Urban intersection"](../golden_scenes/gs1_urban_intersection.md)
- **Release target:** v0.4.0
- **Gap coverage:** [gap 1 — viewport visual completeness](../gap_analysis.md#gap-1--viewport-visual-completeness)

## Scope sketch

Kernel first, then assets, then render
([decomposition rule](../../standards/product-parity.md)):

### Kernel

- OpenDRIVE `<objects>`: parse, represent (arena-stored, generational ids),
  write, validate — initial object classes: crosswalk (outline objects),
  poles/props (point objects with orientation), object repeat records for
  tree lines. Rule-id-cited diagnostics throughout.
- OpenDRIVE `<signals>`: dynamic (traffic lights) and static (signs),
  including `s/t` placement, orientation, and validity records.
- Road-mark completions: stop lines, lane arrows, crosswalk-adjacent
  markings; multi-line road marks (`solid solid` as true dual geometry —
  currently descoped from M2); road-mark color.
- Mesh generation for the above: object placeholder meshes + instanced prop
  anchors, marking geometry for arrows/stop lines.

### Assets

- CC0 prop set: vegetation, poles, signal heads, sign plates
  ([candidates](../asset_candidates.md)); per-file verified sign faces.
- Asphalt/concrete texture set extension beyond the M2 minimum.

### Editor / render

- Textured viewport mode becomes the **default** (sober mode kept as a
  toggle) — extends the optional textured mode scoped in the
  [M2 assets design](../../design/m2/05_assets.md).
- Terrain skirt around the network + procedural ground.
- Sky/lighting pass (procedural sky or CC0 HDRI; simple sun lighting).
- Prop rendering via instancing behind the existing `Renderer` interface.
- Object/signal placement editing: minimal placement + properties-panel
  workflow (full library browsing UX is M4 — see open question 2).

## Dependencies on prior milestones

- M2's editing framework (commands/undo), junction surfaces, and asset
  pipeline are the substrate; GS-1 is authored with M2 tools.
- M2's road-mark width editing; M3a builds the remaining road-mark types on
  that model.

## Top 3 risks

1. **`<objects>`/`<signals>` model breadth** — the OpenDRIVE object/signal
   chapters are large; scoping to what GS-1 needs without painting the data
   model into a corner requires reading the normative chapters first
   ([references](../../domain/references.md)).
2. **Render-path growth** — instancing, textures, terrain, and sky in one
   milestone could destabilize the thin-renderer posture; mitigation:
   everything stays behind `Renderer`, sober mode remains the fallback and
   the packaging smoke path.
3. **Asset style coherence** — mixed-style props make GS-1 read worse than
   no props; mitigation: one primary kit family, style rule recorded in the
   [asset candidates](../asset_candidates.md).

## Open questions for the maintainer

1. Signal *catalog* depth: does M3a model signal types beyond what GS-1
   shows (full country catalogs), or exactly the GS-1 set?
2. Does M3a pull forward a minimal read-only library panel for prop
   placement, or is properties-panel placement acceptable until M4? (See
   [Library Browser placement](../roadmap.md#library-browser-placement).)
3. Shadows in the M3a lighting pass, or defer to a later render milestone?
