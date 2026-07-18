#pragma once

// The Marking-library-item → kernel RoadMark mapping, shared by the viewport
// drop resolver (library_drop.cpp) and the Attributes-pane marking slot
// (properties_panel.cpp) so both agree on the manifest spelling vocabulary —
// the spelling tables live here once, never duplicated. Pure data (no widgets),
// unit-testable headless.

#include "roadmaker/road/lane.hpp"

#include <QString>
#include <optional>

#include "document/library_manifest.hpp"

namespace roadmaker::editor {

/// The RoadMarkType for a manifest mark_type string, or nullopt for a spelling
/// this build does not paint (an unknown type rejects the drop rather than
/// silently mis-marking). Manifest spelling is underscored ("broken_broken");
/// the xodr writer maps it to the ASAM space form ("broken broken").
[[nodiscard]] std::optional<RoadMarkType> mark_type_from_string(const QString& type);

/// The RoadMarkColor for a manifest mark_color string, or nullopt for an unknown
/// spelling. An empty/absent color means "the standard color for this type".
[[nodiscard]] std::optional<RoadMarkColor> mark_color_from_string(const QString& color);

/// The RoadMark a Marking library item authors, or nullopt when its type/color
/// spelling is unknown. `lines` stays empty (compact form — the mesher
/// synthesizes multi-stripe geometry for solid_solid etc.), mirroring the
/// junction centre-double-yellow precedent.
[[nodiscard]] std::optional<RoadMark> mark_from_item(const LibraryItem& item);

} // namespace roadmaker::editor
