// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Headless loader for the runtime library manifest (assets/library/manifest.json)
// — the catalogue of draggable items the Library panel shows and the drop
// handler resolves. Pure data + parsing (Qt JSON, no widget), unit-testable
// offscreen. The schema is versioned and forward-compatible: an unknown
// manifest_version parses best-effort (a warning, not a crash) and an item
// whose create kind this build doesn't understand is kept as Unknown, so a
// newer manifest (Phase 3 props) never breaks an older editor.

#include "roadmaker/error.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
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
    Unknown
  };

  /// One weighted choice in a PropSet: a bundled prop model plus its relative
  /// draw weight. A scatter draws one entry per instance weighted by `portion`.
  struct PropSetEntry {
    QString model;        ///< a bundled prop model id (props::model)
    double portion = 1.0; ///< relative draw weight (must be > 0)
  };

  QString key;       ///< stable id (drag payload / scene reference)
  QString label;     ///< shown in the panel
  QString category;  ///< grouping header ("Road templates", "Road styles", "Assemblies", …)
  QString thumbnail; ///< manifest-relative image path (may be empty/absent on disk)

  Kind kind = Kind::Unknown;
  QString profile;  ///< RoadTemplate: "two_lane_rural" | "urban_sidewalk" | "highway"
  QString style;    ///< RoadStyle: "urban_two_lane" | "two_lane_rural" | "highway"
  QString assembly; ///< Assembly: "t" | "x"
  QString model;    ///< Tree: a bundled prop model id (e.g. "tree_pine")
  QString signal;   ///< Signal: "light" (traffic light) | "sign" (static sign)

  QString mark_type;        ///< Marking: "solid" | "broken" | "solid_solid" | …
  QString mark_color;       ///< Marking: "white" | "yellow" | …
  double mark_width = 0.12; ///< Marking: painted width [m] (OpenDRIVE default)
  QString material;         ///< Material: "asphalt" | "concrete" | …

  /// Crosswalk (parametric asset, p3-s2): stripe geometry + paint material +
  /// segmentation category. Materialized into each placed instance's object.
  double crosswalk_width = 3.0;   ///< walking depth along the road [m]
  double crosswalk_border = 0.0;  ///< edge-line width [m]; 0 = no border
  double crosswalk_dash = 0.5;    ///< stripe length along the crossing [m]; 0 = solid
  double crosswalk_gap = 0.5;     ///< gap between stripes [m]
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
};

class LibraryManifest {
public:
  /// The manifest schema this build understands; higher versions parse
  /// best-effort with a warning.
  static constexpr int kSupportedVersion = 1;

  /// Parses manifest bytes (testable without a file). Errors: malformed JSON,
  /// a missing/invalid `manifest_version`, or a missing `items` array.
  [[nodiscard]] static Expected<LibraryManifest> parse(const QByteArray& json);

  /// Loads and parses a manifest file. Adds an IO error for an unreadable file.
  [[nodiscard]] static Expected<LibraryManifest> load(const std::filesystem::path& path);

  [[nodiscard]] int version() const { return version_; }

  [[nodiscard]] const std::vector<LibraryItem>& items() const { return items_; }

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
  int version_ = kSupportedVersion;
  std::vector<LibraryItem> items_;
};

} // namespace roadmaker::editor

// Lets a LibraryItem cross a queued/introspected signal (PropertiesPanel's
// crosswalk_asset_committed) and be recorded by QSignalSpy in tests.
Q_DECLARE_METATYPE(roadmaker::editor::LibraryItem)
