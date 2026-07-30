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

// Headless loader for the runtime library manifest (assets/library/manifest.json)
// — the catalogue of draggable items the Library panel shows and the drop
// handler resolves. Pure data + parsing (Qt JSON, no widget), unit-testable
// offscreen. The schema is versioned and forward-compatible: an unknown
// manifest_version parses best-effort (a warning, not a crash) and an item
// whose create kind this build doesn't understand is kept as Unknown, so a
// newer manifest (Phase 3 props) never breaks an older editor.

#include "roadmaker/error.hpp"
#include "roadmaker/road/defaults.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <array>
#include <filesystem>
#include <vector>

namespace roadmaker::editor {

/// One catalogue entry. `create` is a tagged union of what a drop produces:
/// a road template (arms Create Road with `profile`) or a parametric assembly
/// (`assembly` = "t" | "x"). Kind::Unknown = a create kind from a newer
/// manifest this build can't act on (shown but not droppable).
struct LibraryItem {
  enum class Kind {
    RoadTemplate,
    RoadStyle,
    /// ★ A ROAD JUNCTION, not a prop. `create.kind == "assembly"` has meant the
    /// parametric T/X intersection template since it shipped
    /// (edit::assembly::t_intersection), and the user guide defines Assemblies
    /// that way. A composite PROP assembly is `PropAssembly` below — the two
    /// are unrelated and must never be merged.
    Assembly,
    /// The create-intent tag for EVERY point prop — trees, shrubs, streetlights
    /// and buildings alike (create.kind == "tree" in the manifest). The actual
    /// OpenDRIVE object class each places (Tree/Vegetation/Pole/Building) comes
    /// from the bundled model (props::model(id)->type), not from this tag.
    Tree,
    Signal,
    Marking,
    Material,
    Crosswalk,
    Stencil,
    PropSet,
    /// A COMPOSITE PROP — several prop models pinned together by fixed relative
    /// transforms, placed as one unit (p6-s9, #323). `create.kind ==
    /// "prop_assembly"`, `create.prop_assembly` names a `props::assembly()` id.
    /// Distinct from `Assembly` above (road junctions) and from `PropSet`
    /// (a weighted RANDOM scatter — one model drawn per instance, where an
    /// assembly places every part, every time, at its authored offset).
    PropAssembly,
    Unknown
  };

  /// One weighted choice in a PropSet: a bundled prop model plus its relative
  /// draw weight. A scatter draws one entry per instance weighted by `portion`.
  struct PropSetEntry {
    QString model;        ///< a bundled prop model id (props::model)
    double portion = 1.0; ///< relative draw weight (must be > 0)
    /// Transient uniform spawn multiplier, DERIVED from the merged library at
    /// arm time (LibraryListModel::default_scale_for_model) — never parsed from
    /// or serialized to the manifest. Lets a scatter honor each drawn model's
    /// Default scale (a mixed pine+birch set spawns both at their own defaults).
    double default_scale = 1.0;
  };

  QString key;       ///< stable id (drag payload / scene reference)
  QString label;     ///< shown in the panel
  QString category;  ///< grouping header ("Road templates", "Road styles", "Assemblies", …)
  QString thumbnail; ///< manifest-relative image path (may be empty/absent on disk)

  Kind kind = Kind::Unknown;
  QString profile;  ///< RoadTemplate: "freeway" | "arterial" | "collector" | "local"
  QString style;    ///< RoadStyle: "freeway" | "arterial" | "collector" | "local"
  QString assembly; ///< Assembly: "t" | "x" — a road junction shape, not a prop
  QString model;    ///< Tree: a bundled prop model id (e.g. "tree_pine")
  QString signal;   ///< Signal: "light" (traffic light) | "sign" (static sign)
  /// PropAssembly: a `props::assembly()` id (e.g. "signal_mast"). Resolved at
  /// DROP time rather than at parse time, unlike a PropSet's entries: a project
  /// assembly only exists once its overlay is registered, so a row parsed before
  /// the project opened must not be discarded for naming an id that is about to
  /// exist.
  QString prop_assembly;

  /// Uniform spawn multiplier applied to a prop's bundled model dims at
  /// placement (make_prop_object scales BOTH height and radius). 1.0 = the
  /// model's native size. Prop (Kind::Tree) kinds only; ignored otherwise.
  double default_scale = 1.0;

  QString mark_type;  ///< Marking: "solid" | "broken" | "solid_solid" | …
  QString mark_color; ///< Marking: "white" | "yellow" | …
  /// Marking: painted width [m] (the registry's normal-line width).
  double mark_width = roadmaker::defaults::kLineWidth;
  QString material; ///< Material: "asphalt" | "concrete" | …

  /// Crosswalk (parametric asset, p3-s2): stripe geometry + paint material +
  /// segmentation category. Materialized into each placed instance's object.
  double crosswalk_width = roadmaker::defaults::kCrosswalkWidth; ///< walking depth [m]
  double crosswalk_border = 0.0; ///< edge-line width [m]; 0 = no border
  /// Zebra bar length along the crossing [m] (realism_defaults.md §1.3); 0 = solid.
  double crosswalk_dash = roadmaker::defaults::kCrosswalkStripeLength;
  double crosswalk_gap = roadmaker::defaults::kCrosswalkStripeGap; ///< gap between stripes [m]
  QString crosswalk_material;     ///< paint material code (e.g. "material.paint_white")
  QString crosswalk_segmentation; ///< segmentation category tag

  /// Stencil (point arrow asset, p3-s4): glyph subtype + geometry + paint
  /// material + segmentation category. Materialized into each placed instance's
  /// cornerLocal outline; the width scales to the picked lane by `stencil_width_frac`.
  QString stencil_subtype;         ///< one of the 6 core arrow subtypes
  double stencil_length = 4.0;     ///< glyph extent along travel [m]
  double stencil_width_frac = 0.5; ///< glyph width as a fraction of the lane width
  QString stencil_material;        ///< paint material code (e.g. "material.paint_white")
  QString stencil_segmentation;    ///< segmentation category tag

  /// PropSet (weighted scatter asset, p6-s5): the model choices a scatter draws
  /// from. Entries that don't resolve to a bundled model, or whose portion is
  /// not positive, are dropped on parse. A resolved draw yields a synthetic
  /// Tree item (see prop_placement::resolve_prop_asset).
  std::vector<PropSetEntry> prop_entries;

  /// The item's verbatim `create` JSON block. Captured on parse so an unknown
  /// create kind — or a modeled one carrying forward-compat fields this build
  /// doesn't understand — round-trips byte-for-byte through to_json(). Empty
  /// for a programmatically built item, which to_json() then serializes from
  /// the modeled fields above.
  QJsonObject create_raw;

  /// True for a row this build INVENTED from a `materials[]` entry rather than
  /// reading from `items[]` (p6-s8, #322). Such a row is a view onto the
  /// material definition, so to_json() must not write it back — otherwise every
  /// save would duplicate the definition into `items[]` and the two copies would
  /// drift. Never parsed, never serialized.
  bool synthesized = false;
};

/// One entry of the manifest's `materials[]` block — a PBR-lite material
/// definition (schema: docs/design/materials-structures/01_material_system.md §2,
/// shipped by p6-s8, #322).
///
/// Definitions are PROJECT ASSETS, not scene data: a `.xodr` stores only the
/// `id`, never a texture path, so two projects sharing a scene cannot disagree
/// about what `rm:asphalt_new` looks like.
struct LibraryMaterial {
  /// The contract. What a `.xodr` stores in `<material surface>` and what the
  /// renderer looks up, namespaced `rm:` so it is recognisable as ours in a
  /// foreign file. Stored verbatim as written; the catalog's lookup strips the
  /// prefix, so `rm:x`, `x` and `material.x` all resolve.
  QString id;
  QString label;
  QString category;
  QString thumbnail; ///< manifest-relative image path

  /// `maps{}` — project-relative paths, any of which may be empty. Missing
  /// normal/roughness fall back to the scalar params; a missing albedo falls
  /// back to the mesh's flat colour, which is what keeps old scenes unchanged.
  QString albedo;
  QString normal;
  QString roughness;

  double uv_scale = 0.25; ///< texels per metre

  /// `params{}` — scalar fallbacks and modifiers, applied whether or not the
  /// corresponding map exists.
  double param_roughness = 0.8;
  double normal_strength = 1.0;
  std::array<double, 4> tint{1.0, 1.0, 1.0, 1.0};
  /// Nominal friction, authored into the lane's `<material>` record. A
  /// RoadMaker addition to the design doc's `params` list, because §3 of that
  /// doc already assumes a friction reaches the `.xodr` (amended with #322).
  double friction = 0.9;

  /// Provenance for an imported material (#322's acceptance): the absolute path
  /// it was copied FROM, and the user's licence attestation. Never read to load
  /// the asset — the copy inside the project is what is used. Empty on a
  /// compiled-in material, whose licence is ledgered in ASSETS_LICENSES.md.
  QString source;
  QString license;

  /// The verbatim entry, so forward-compat fields this build does not model
  /// round-trip untouched — the same contract LibraryItem::create_raw carries.
  QJsonObject raw;
};

class LibraryManifest {
public:
  /// The manifest schema this build understands; higher versions parse
  /// best-effort with a warning.
  ///
  /// v1 -> v2 (p6-s8, #322): the `materials[]` block the material design doc
  /// committed. A v1 manifest still parses in a v2 build — no `materials[]`
  /// simply means no project materials — and a v2 manifest in a v1 build shows
  /// its items and ignores the block, which is the forward compatibility that
  /// was already designed in and must not regress.
  static constexpr int kSupportedVersion = 2;

  /// Parses manifest bytes (testable without a file). Errors: malformed JSON,
  /// a missing/invalid `manifest_version`, or a missing `items` array.
  [[nodiscard]] static Expected<LibraryManifest> parse(const QByteArray& json);

  /// Loads and parses a manifest file. Adds an IO error for an unreadable file.
  [[nodiscard]] static Expected<LibraryManifest> load(const std::filesystem::path& path);

  [[nodiscard]] int version() const { return version_; }

  [[nodiscard]] const std::vector<LibraryItem>& items() const { return items_; }

  /// The project's material definitions (`materials[]`, p6-s8 #322). Empty for a
  /// v1 manifest, and for a v2 one that defines none.
  [[nodiscard]] const std::vector<LibraryMaterial>& materials() const { return materials_; }

  /// Adds or replaces a material definition, matched on `id`. Also refreshes the
  /// synthesized catalogue row that presents it in the Library.
  void upsert_material(LibraryMaterial material);

  /// Removes the definition with `id` (and its synthesized row). False when
  /// there was none.
  bool remove_material(const QString& id);

  /// Serializes the manifest back to JSON bytes. A parsed item re-emits its
  /// verbatim `create` block (create_raw), so unknown kinds and forward-compat
  /// fields survive the round-trip; a programmatically built item (empty
  /// create_raw) is serialized from its modeled fields.
  [[nodiscard]] QByteArray to_json() const;

  /// Atomically writes to_json() to `path` (QSaveFile, temp-then-rename — the
  /// Project::create pattern). Errors on a write/commit failure.
  [[nodiscard]] Expected<void> save(const std::filesystem::path& path) const;

  /// Adds `item`, or replaces the item with the same key in place.
  void upsert(LibraryItem item);

  /// Removes the item with `key`; returns true if one was removed.
  bool remove(const QString& key);

private:
  /// Rebuilds the synthesized `items_` rows that present `materials_` in the
  /// Library, so a material definition needs no hand-written `items[]` entry.
  void resync_material_rows();

  int version_ = kSupportedVersion;
  std::vector<LibraryItem> items_;
  std::vector<LibraryMaterial> materials_;
};

} // namespace roadmaker::editor

// Lets a LibraryItem cross a queued/introspected signal (PropertiesPanel's
// crosswalk_asset_committed) and be recorded by QSignalSpy in tests.
Q_DECLARE_METATYPE(roadmaker::editor::LibraryItem)
