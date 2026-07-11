# M3a assets — CC0 prop set, textures, and the runtime library manifest

Policy (unchanged, inherited from [m2/05](../m2/05_assets.md) and the
[asset standard](../../standards/assets.md)): **CC0 / MIT / ISC / Apache-2.0
only**, verified on the source page **at fetch time**, every file recorded in
`ASSETS_LICENSES.md`, fetched files additionally pinned in `assets/manifest.json`
(url + sha256 + license_url), `scripts/check_asset_licenses.py` gating CI.
US-federal public-domain works count as CC0. **No CC-BY without explicit
maintainer approval; never CC-BY-NC/ND or unlicensed.** M3a adds 3D models and
extends textures — same pipeline, no exceptions.

This doc has two deliverables: (1) the GS-1 prop/texture inventory and (2) the
**runtime library manifest** — the format the editor's read-only library panel
and the export path consume to place and resolve assets. Decision 2
([#34](https://github.com/Robomous/RoadMaker/issues/34)) commits this format in
M3a; the M4 browser must subsume it (§4 risk 4).

## 1. GS-1 asset inventory

Scouted candidates: [asset_candidates.md](../../roadmap/asset_candidates.md).
M3a selects **one primary low-poly kit family** for GS-1 to avoid the
style-incoherence risk (`00` risk 3). Selection is made at fetch time and
recorded; the design assumes Kenney (City + Nature kits, CC0) as primary with
Quaternius as fallback for gaps, but the choice is confirmed during Phase 3.

| Class | GS-1 need | Primary source | License | Notes |
|---|---|---|---|---|
| Vegetation | ~20 street trees + shrubs | Kenney Nature Kit | CC0 | Trees are style-sensitive — one family only |
| Poles | signal/sign carriers | Kenney City Kit | CC0 | Style-neutral; may mix families |
| Signal heads | 4× traffic-light head | Kenney City Kit / original | CC0 | Head + pole may be one model or composed |
| Sign plates | speed-limit, ped-crossing | sign face + generic plate | CC0 / US-federal PD | Faces verified **per file** (§2) |
| Asphalt / concrete / grass | roadway, sidewalk, terrain | ambientCG 1K | CC0 | Extends the M2 minimum; grass stays procedural-first |
| Sky | daytime lighting | procedural (no fetch) or Poly Haven HDRI | CC0 | HDRI only if procedural reads poorly (`04` §2) |

Every selected file gets an `ASSETS_LICENSES.md` row and (if fetched) a
`manifest.json` entry in the **same commit** as its use — the standing rule.

## 2. Sign faces — per-file verification (hard rule)

Traffic-sign graphics are the licensing minefield. Mandate (from
[asset_candidates](../../roadmap/asset_candidates.md)):

- **US MUTCD** graphics are US-federal public domain → CC0-equivalent, usable
  per file after verification.
- **Vienna-convention SVGs on Wikimedia Commons** are licensed **per file**
  (PD-self / CC0 / CC-BY mixtures). Each file is verified and recorded
  individually; CC-BY files are **skipped or escalated** for maintainer
  approval; **never bulk-import a category**.
- GS-1 needs exactly two sign faces (speed-limit 50, pedestrian-crossing
  warning). Verify and record those two; nothing more.

## 3. Model format & pipeline

- **Format:** glTF 2.0 (`.glb`, self-contained) — the project already speaks
  glTF on the export side; using it for source props means one loader and a
  known Y-up→Z-up convention (convert at load, mirroring the export boundary
  rule). Kit files distributed as OBJ/FBX are re-encoded to `.glb` via a
  documented `postprocess` step (Blender headless or `gltf` CLI); FBX SDK is
  **forbidden** (proprietary) — use the kit's glTF export or an intermediate,
  never the Autodesk SDK.
- **Destination:** `assets/models/<class>/<name>.glb`; textures under
  `assets/textures/<material>/`. Re-encode to project conventions (1K, power-of-
  two) per the M2 pipeline rules.
- **Fetch:** extend `scripts/fetch_assets.py` — it already validates
  `sha256`/`license`/`license_url`. Models are just new manifest entries; no
  script change beyond allowing the `assets/models/` destination and `.glb`.
- **Provenance:** `manifest.json` gains an optional `postprocess` list per entry
  (already supported as informational) documenting re-encode steps so the
  binary is reproducible from the pinned source.

## 4. Runtime library manifest (committed format)

Separate from `assets/manifest.json` (which is a *fetch/provenance* ledger for
CI), the **runtime library manifest** is what the editor loads to populate the
library panel and what the object/signal placement resolves an asset reference
against. New file: `assets/library.json`.

```jsonc
{
  "manifest_version": 1,            // bumped when the schema changes; M4 extends
  "generated": "YYYY-MM-DD",
  "items": [
    {
      "key": "tree.oak",            // stable id referenced by the scene / <object>
      "label": "Oak tree",          // shown in the flat list (M3a); M4 adds search
      "class": "vegetation",        // vegetation | pole | signal_head | sign | prop
      "model": "assets/models/vegetation/oak.glb",
      "odr": {                      // how a placed instance maps to OpenDRIVE
        "element": "object",        // "object" | "signal"
        "type": "tree",             // <object @type> / <signal @type>
        "subtype": "",
        "bounding": { "radius": 2.0, "height": 7.5 }
      },
      "placement": {                // editor defaults on drag-to-place
        "z_offset": 0.0,
        "orientation": "none",
        "snap": "sidewalk"          // lane-type snap hint; "" = free
      },
      "license_ref": "assets/models/vegetation/oak.glb"  // → ASSETS_LICENSES row
    }
  ]
}
```

Design rules that keep it M4-extensible (risk 4):

- **`manifest_version`** gates schema evolution; M4's browser reads v1 and adds
  fields (thumbnails, categories, tags, search index) under higher versions
  without breaking M3a scenes.
- **No editing semantics** in the manifest — it is read-only metadata. Placement
  *defaults* are hints, not constraints; the actual authored values live in the
  `<object>`/`<signal>` the placement command creates.
- **`key` is the stable contract.** A placed instance stores the library `key`
  (in the editor's scene model and, for export, as a `<userData code="rm:asset">`
  on the `<object>` so a re-opened scene re-resolves the model). The `.xodr`
  stays standards-valid without the library — the `key` is an additive hint, the
  `<object>` geometry/type is authoritative (mirrors the M2 `rm:waypoints`
  pattern).
- **Loader:** a headless `LibraryManifest` reader in the editor's document layer
  (`editor/src/document/`), testable without a window, with a
  `QAbstractItemModel` view for the panel shipped with its
  `QAbstractItemModelTester` gtest (standing rule).

## 5. Test plan

- `check_asset_licenses.py` green: every new file has an `ASSETS_LICENSES.md`
  row; every fetched file a manifest entry with a valid license.
- `library.json` schema test: loader parses v1, rejects unknown
  `manifest_version` gracefully (forward-compat warning, not a crash), resolves
  every `model`/`license_ref` path to an existing file.
- Model load test: each `.glb` loads headless and reports a non-empty mesh
  (guards against a corrupt re-encode).
- Style/coherence is a manual review gate at Phase 3 (one primary family),
  recorded in the PR.
</content>
