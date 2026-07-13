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
#include <QString>
#include <filesystem>
#include <vector>

namespace roadmaker::editor {

/// One catalogue entry. `create` is a tagged union of what a drop produces:
/// a road template (arms Create Road with `profile`) or a parametric assembly
/// (`assembly` = "t" | "x"). Kind::Unknown = a create kind from a newer
/// manifest this build can't act on (shown but not droppable).
struct LibraryItem {
  enum class Kind { RoadTemplate, Assembly, Unknown };

  QString key;       ///< stable id (drag payload / scene reference)
  QString label;     ///< shown in the panel
  QString category;  ///< grouping header ("Road templates", "Assemblies", …)
  QString thumbnail; ///< manifest-relative image path (may be empty/absent on disk)

  Kind kind = Kind::Unknown;
  QString profile;  ///< RoadTemplate: "two_lane_rural" | "urban_sidewalk" | "highway"
  QString assembly; ///< Assembly: "t" | "x"
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

private:
  int version_ = 0;
  std::vector<LibraryItem> items_;
};

} // namespace roadmaker::editor
