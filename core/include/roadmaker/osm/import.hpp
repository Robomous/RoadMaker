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

#include "roadmaker/edit/command.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/osm/network_plan.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osm {

/// Diagnostics an import can only learn while APPLYING — a junction whose
/// generator dropped turns, a link `check_linkable` refuses. The caller owns
/// the sink and reads it after the push; apply() clears it first, so a redo
/// re-reports rather than doubling.
///
/// A shared_ptr rather than a reference because the command outlives the call
/// that built it (`EditStack::push` takes ownership), which is the same reason
/// `EditStack::last_follow_records()` exists at all.
using ApplyDiagnostics = std::shared_ptr<std::vector<Diagnostic>>;

/// Turns a plan into ONE undoable command.
///
/// Roads are authored first, then joints — the `make_intersection` ordering,
/// and necessary because a joint names roads by plan index and can only
/// resolve them once they exist.
///
/// **Every stage is tolerant.** `edit::composite` unwinds its whole prefix on
/// a failure, which is right for a four-arm intersection and exactly wrong
/// here: one un-fittable way must not discard sixteen hundred good roads. Each
/// stage therefore catches its child's error, records it against the way id,
/// and succeeds having done nothing. That deviation is stated rather than
/// silent, because "atomic" is the composite's advertised contract.
[[nodiscard]] RM_API std::unique_ptr<edit::Command>
import_plan(const RoadNetwork& network, NetworkPlan plan, ApplyDiagnostics sink = {});

/// Everything an import needs, prepared but not yet applied.
struct OsmImport {
  std::unique_ptr<edit::Command> command;
  NetworkPlan plan;
  /// Parse and plan diagnostics — available BEFORE the network changes, which
  /// is what lets a caller show the compromises and then decide.
  std::vector<Diagnostic> diagnostics;
  /// Filled during apply; empty until then.
  ApplyDiagnostics apply_diagnostics;
};

/// The one call the editor and Python make: read, reproject, plan, and build
/// the command.
///
/// The scene must already have a georeference. The refusal when it does not is
/// `gis::crs_transform`'s own, VERBATIM and shared with the GIS and lidar
/// importers — this function never sets an origin itself, because silently
/// georeferencing a scene gives it a projection the user never chose.
[[nodiscard]] RM_API Expected<OsmImport> prepare_osm_import(const RoadNetwork& network,
                                                            const std::filesystem::path& path,
                                                            const OsmBuildOptions& options = {});

} // namespace roadmaker::osm
