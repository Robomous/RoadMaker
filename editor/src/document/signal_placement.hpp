#pragma once

// Signal placement helpers (p4-s7, issue #228), shared by the Library
// signal-drop path and the Signal tool so both funnel through identical
// snapping and identical id-minting — the prop_placement.hpp precedent, applied
// to the OTHER road-relative entity.
//
// Before this split the drop path owned a private `make_dropped_signal` and its
// own snap threshold (library_drop.cpp), so a tool placing a signal would have
// had to re-derive both and could have drifted from the drop. Pure data over the
// kernel's signal model — no widgets, no kernel changes — so it is unit-testable
// headless.

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/signal.hpp"

#include <QString>
#include <array>
#include <optional>
#include <string>

#include "document/library_manifest.hpp"
#include "document/prop_placement.hpp" // RoadStation, nearest_road_station

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// A signal snaps to a road within this lateral distance [m] of its reference
/// line — same rationale as the prop threshold (OpenDRIVE signals are
/// road-relative; a placement in open space is rejected rather than invented).
inline constexpr double kSignalSnapThreshold = 12.0;

/// The nearest road to (x, y) within kSignalSnapThreshold, with the placement's
/// road-relative (s, t). nullopt when no road is in reach. The ONE snapping
/// entry point for signals: the drop and the tool both call it, so they cannot
/// disagree about where a road ends.
[[nodiscard]] std::optional<RoadStation>
nearest_signal_station(const RoadNetwork& network, double x, double y);

/// Lowest positive integer odr id not already used by a signal (id-unique per
/// `<signals>`). Signals and objects have separate id spaces in OpenDRIVE, which
/// is why this is not next_object_odr_id.
[[nodiscard]] std::string next_signal_odr_id(const RoadNetwork& network);

/// True when `item` is a Library signal asset (manifest kind "signal") — what
/// the tool refuses to place from before it touches the network.
[[nodiscard]] bool is_signal_asset(const LibraryItem& item);

/// The `Signal` a library item's `signal` tag authors at road-relative (s, t).
///
/// "light" is a dynamic traffic light (OpenDRIVE catalog type); the sign tags
/// are static German StVO (VzKat) regulatory plates the mesh builder renders by
/// their catalog type: "sign_stop" → 206 (STOP), "sign_yield" → 205 (yield),
/// and the generic "sign" → 274/50 (speed-limit-50, a recognisable default the
/// user can retype). Orientation "+" faces the signal along increasing s.
[[nodiscard]] Signal make_signal(const QString& tag, std::string odr_id, double s, double t);

/// World position of a signal head: its (s, t) on the owning road, lifted by its
/// own zOffset — the exact projection the mesh builder instances the head at, so
/// an overlay marker never drifts from the rendered pole. `nullopt` when the
/// signal, its road, or the road's geometry is gone. Shared by the Signal tool's
/// structural overlay and p4-s8's phase overlay so the two never disagree about
/// where a head is.
[[nodiscard]] std::optional<std::array<double, 3>> signal_world(const RoadNetwork& network,
                                                                SignalId id);

} // namespace roadmaker::editor
