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

// Imported GIS data shown under the network as authoring reference
// (p7-s2, #242).
//
// ★ WHY THIS IS NOT AN edit::Command, AND NOT KERNEL STATE.
//
// ADR-0008 names "library/asset references and import metadata" as Layer 2:
// state that lives in the native container and NEVER enters the `.xodr`. An
// imagery underlay is a backdrop to trace over, not something a simulator
// consuming the exported network could use — so it is editor state, held here,
// and adding or removing one is deliberately NOT undoable. That is the same
// ruling `WorldGeoreferenceWindow::fit_workspace_to_selection` already carries
// for the workspace box, for the same reason.
//
// The ONE exception is an elevation raster, which does not become a reference
// layer at all: it becomes the network's HeightField through the ordinary
// `edit::set_terrain_field` command, and is therefore real scene content,
// undoable, and persisted in the `.xodr`'s terrain sidecar like any other
// terrain. Import DEM does not appear in this file.
//
// ★ PATHS ARE RELATIVE, and that is a contract. Every external reference in
// this repository resolves against its owning document's directory
// (`is_safe_sidecar_reference` is the kernel's version of the rule). A project
// copied to another machine, or committed to a repository, must keep working.

#include "roadmaker/gis/layer.hpp"
#include "roadmaker/gis/reproject.hpp"
#include "roadmaker/road/georeference.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace roadmaker::editor {

enum class ReferenceLayerKind {
  Vector,
  Raster,
};

/// One imported reference layer.
///
/// The persisted half is `path`, `kind`, `visible` and `framed_crs`; everything
/// else is recomputed on load. Storing the reprojected geometry would mean a
/// sidecar that silently disagrees with its own source file after either one
/// changes.
struct ReferenceLayer {
  /// Relative to the scene's directory, using '/' separators so a project made
  /// on Windows opens on Linux. Never absolute.
  std::string path;

  ReferenceLayerKind kind = ReferenceLayerKind::Raster;
  bool visible = true;

  /// The scene's projection string AS IT WAS when this layer was placed.
  ///
  /// Same rule, and the same reason, as `SceneWorkspaceState::crs`: reprojected
  /// coordinates only mean something in one frame. If the scene's
  /// `<geoReference>` has changed since, the placement describes somewhere
  /// else, and the honest move is to re-derive it from the source file rather
  /// than draw the user's imagery over the wrong ground.
  std::string framed_crs;

  // --- Recomputed on load; never persisted -------------------------------

  /// The CRS the source file itself declares, for the panel's read-out.
  std::string source_crs;

  gis::GisVectorLayer vector;
  gis::PlacedRaster raster;

  /// False when the file was missing or unreadable. Such a layer is KEPT in
  /// the list rather than dropped: silently losing a reference the user added
  /// because a drive was not mounted is worse than showing it as unavailable.
  bool loaded = false;

  /// One line for the panel: "UTM zone 31N · placed", or why it failed.
  std::string status;

  [[nodiscard]] bool drawable() const { return loaded && visible; }
};

/// The scene's reference layers, and the loading that fills them in.
///
/// A plain value type with free functions rather than a QObject: this is
/// document state, and `Document` owns the signalling — the same split the rest
/// of `editor/src/document/` uses so the model stays headless-testable.
class ReferenceLayers {
public:
  [[nodiscard]] const std::vector<ReferenceLayer>& layers() const { return layers_; }

  [[nodiscard]] std::size_t size() const { return layers_.size(); }

  [[nodiscard]] bool empty() const { return layers_.empty(); }

  [[nodiscard]] const ReferenceLayer& at(std::size_t index) const { return layers_.at(index); }

  /// Adds a layer for `source`, which may be anywhere on disk; it is stored
  /// relative to `scene_dir`. Returns the diagnostics the read produced.
  ///
  /// Fails when the file cannot be read or cannot be reprojected into the
  /// scene's frame — the caller surfaces that, because a refusal that names the
  /// CRS is the whole point of the bounded family.
  Expected<std::vector<Diagnostic>> add(const std::filesystem::path& source,
                                        const std::filesystem::path& scene_dir,
                                        const GeoReference& scene_georeference);

  void remove(std::size_t index);
  void set_visible(std::size_t index, bool visible);
  void clear();

  /// Replaces the list with `persisted` and re-reads every source file against
  /// the scene's CURRENT georeference. A layer whose file is gone, or whose
  /// frame has changed incompatibly, comes back `loaded == false` with a
  /// diagnostic rather than being dropped.
  std::vector<Diagnostic> reload(std::vector<ReferenceLayer> persisted,
                                 const std::filesystem::path& scene_dir,
                                 const GeoReference& scene_georeference);

  /// Re-derives every layer's placement against a georeference that has just
  /// changed. Called when the world georeference is edited, so imagery follows
  /// the frame instead of staying where the old one put it.
  std::vector<Diagnostic> refit(const std::filesystem::path& scene_dir,
                                const GeoReference& scene_georeference);

  /// Plan-view extent of every drawable layer, or nullopt when there is none.
  [[nodiscard]] std::optional<std::array<double, 4>> bounds() const;

private:
  std::vector<ReferenceLayer> layers_;
};

/// Relative path from `base` to `target`, with '/' separators. Falls back to
/// the absolute path when the two are on different roots (a different Windows
/// drive), because a broken relative path would be worse than an honest
/// machine-specific one.
[[nodiscard]] std::string relative_reference(const std::filesystem::path& target,
                                             const std::filesystem::path& base);

} // namespace roadmaker::editor
