# Materials & Structures seed — visual depth

*Scope sketch for the Materials & Structures milestone (v0.7.0). A seed, not a
design.*

> **Superseded by the design docs (2026-07-15).** The planning pass ran after
> GS-1 closed, per the gate below. The scope source of truth is now
> **[docs/design/materials-structures/](../../../../design/materials-structures/00_overview.md)**
> (overview · material system · bridge generator · city props · phases);
> workstreams WS-1…WS-5 are issues #196–#200 under epic
> [#183](https://github.com/Robomous/RoadMaker/issues/183). This seed stays as
> the record of the original intent — on conflict, the design docs win.
>
> Two of the seed's open questions were **decided** during that pass and are
> flagged on the epic: **material persistence** (standard `<material>` +
> `<userData>`, no sidecar — ASAM §7.2 names road textures as its example use,
> and M2's `.xodr`-is-the-project-file rule rules out a companion file) and
> **sign-text rendering** (stretch, not committed scope).

- **Theme:** visual depth — the surfaces and structures that make a scene read
  as a real place, not a textured ribbon.
- **Golden scene:** [GS-4 "Rural overpass"](../golden_scenes/gs4_rural_overpass.md)
- **Release target:** v0.7.0
- **Gap coverage:** [gap 6 — material depth and built structures](../gap_analysis.md#gap-6--material-depth-and-built-structures)
  (the three commercial-benchmark clusters); **prerequisite for GS-2's approval**.

## Scope sketch

### Kernel

- **Bridge structure representation** — a data model for a bridge deck (with
  thickness), abutments/piers, and guardrails, generated over a grade
  separation. Read the ASAM OpenDRIVE `objects` / `bridge` chapters at planning
  time and cite the normative rule ids: an OpenDRIVE `<bridge>` records the
  span; the deck/abutment/guardrail *solids* are editor/render geometry
  (Manifold), not necessarily serialized to xodr — **flag the persistence
  question below**.
- **Material assignment model** — how a surface (lane, structure) references a
  material. **Open question (flag, don't decide):** persisted as OpenDRIVE
  `<material>` road records vs. editor-side metadata.

### Editor

- **Material system v2 (PBR-lite)** — albedo + normal + roughness in the GL 3.3
  renderer. "Lite" is defined strictly: **no image-based lighting, no shadow
  maps** (consistent with the M3a lighting decision). Assignable **material
  library** (a Library category): drag a material onto a surface, or assign via
  the Properties panel; variants such as new vs. worn asphalt.
- **Bridge structure generator** — auto-offered when the editor detects a grade
  separation (one road passing over another), and available manually. v1 emits
  deck + abutments/piers + guardrails only.
- **City props** — buildings and streetlights enter as **Library props**
  (CC0), not procedural generation; scouted for GS-2's district density.
- **Sign-text rendering** (text-to-texture) — stretch goal.

### Assets

- CC0 material texture sets (albedo/normal/roughness) and CC0 building/
  streetlight props. Preference order and provenance per
  [assets standard](../../../../standards/assets.md) (CC0 first, then procedural, then
  AI-generated for missing variants). Repo-weight budget: **≤ 512 KB per map,
  ≤ 3 maps per material.**

## Dependencies on prior milestones

- **Needs GS-1's material base** (M3a textured mode: the `Material`/texture
  renderer foundation, per-lane surface classification).
- M2 command layer (material assignment and structure generation are undoable
  commands).
- **Blocks GS-2 approval** — the imported district is unapprovable without
  buildings and material variety.

## Top 3 risks

1. **PBR-lite scope creep** — "PBR" invites IBL, shadows, and a full material
   graph. Mitigation: the strict "lite" definition above (no IBL, no shadows);
   anything past albedo/normal/roughness is a later milestone.
2. **Bridge OpenDRIVE representation** — how much of the structure is normative
   xodr vs. editor geometry. Mitigation: read the `objects`/`bridge` chapters at
   planning time, cite rule ids, and decide persistence explicitly (the flagged
   open question) before building the generator.
3. **Repo weight** — material texture sets bloat the repository. Mitigation: the
   ≤ 512 KB/map, ≤ 3 maps/material budget, enforced by the asset-license check.

## Open questions for the maintainer

1. Guardrail style count for v1 (one generic profile, or a small set)?
2. Material assignment persisted as OpenDRIVE `<material>` records or editor
   metadata? (Flagged, not decided — a standards-vs-tooling call.)
3. Is sign-text rendering in the milestone's committed scope or a stretch that
   slips to a later pass?

## Approved defaults (editable at planning time)

- Bridge v1 = deck + abutments/piers + guardrails only.
- Buildings enter as **Library props, not procedural generation.**
