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

// The per-scene Layer-2 sidecar (fmt-s1, #325; ADR-0008): `<scene>.rmscene.json`
// beside `<scene>.xodr`, carrying everything that has no business inside an
// ASAM file — today the camera/view state and the per-scene render mode.
//
// The compatibility contract this file exists to keep (ADR-0008 "Compatibility
// contract", tested by test_scene_sidecar.cpp / test_scene_state.cpp):
//   - a pure `.xodr` opens and edits with no sidecar at all;
//   - a save writes the sidecar atomically, and never a byte into the `.xodr`;
//   - a missing, stale or malformed sidecar degrades to defaults — it never
//     blocks a load and never costs scene content.
//
// "Stale" means MISSING OR UNPARSEABLE and nothing else. A camera parked far
// from the geometry is legitimate authoring, so there is deliberately no
// geometric staleness detection and no content hash tying a sidecar to its
// scene — do not add one without revisiting the ADR.
//
// QtCore JSON only (no widget, no GL), so it unit-tests offscreen. Versioned
// and forward-compatible like LibraryManifest: a newer `scene_version` parses
// the fields this build knows (a warning, not an error) and — unlike
// project.json v1 — every unknown key survives a rewrite verbatim.

#include "roadmaker/error.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "render/projection_mode.hpp"

// For ReferenceLayerKind. The persisted kind and the runtime kind are the SAME
// enum deliberately: a third spelling added to one and not the other is exactly
// how a layer comes back as the wrong thing after a save.
#include "document/reference_layers.hpp"

// For EditorMode, on the same principle: one enum, persisted and live.
#include "document/editor_mode.hpp"

namespace roadmaker::editor {

/// The camera pose a scene reopens at, in the kernel frame (right-handed,
/// Z-up, meters/radians) — exactly the state OrbitCamera::set_pose() restores.
struct SceneViewState {
  std::array<float, 3> target{0.0F, 0.0F, 0.0F};
  float yaw = 0.0F;
  float pitch = 0.0F;
  float distance = 0.0F;
  ProjectionMode projection = ProjectionMode::Perspective;
};

/// The workspace: the working area of the scene, as a plan-view box in the
/// kernel frame (p7-s5, #324).
///
/// Layer 2 by ADR-0008, which names "workspace extents and georeference
/// framing" as residents of the native container. It is framing, not content —
/// losing it costs a view, never a road.
///
/// `crs` is the scene's projection string AS IT WAS when the box was framed.
/// It is stored because the box is only meaningful in one frame: if the
/// `.xodr`'s `<geoReference>` has since changed, these coordinates describe
/// somewhere else, and the honest thing is to drop them rather than frame the
/// user's workspace on a place that no longer exists. Empty when the scene had
/// no georeference, which is a frame like any other.
struct SceneWorkspaceState {
  /// {west, south, east, north}, metres in the kernel frame — the same
  /// convention as network_plan_bounds and the `<header>` bounding box.
  std::array<double, 4> extents{};
  std::string crs;
};

/// One imported reference layer, as persisted (p7-s2, #242).
///
/// Only what cannot be recomputed. The reprojected geometry and the placed
/// raster are deliberately NOT stored: they are derived from the source file
/// and the scene's georeference, and a cached copy of a derivation is a copy
/// that can disagree with both of its inputs.
struct SceneReferenceLayer {
  /// Relative to the scene's directory, '/' separators. Never absolute — the
  /// same contract every external reference in this repository keeps, so a
  /// project survives being copied or committed.
  std::string path;

  /// "vector", "raster" or "point_cloud" on disk; the reader maps anything else
  /// to raster and warns, rather than dropping a layer over a spelling.
  ///
  /// This was a `bool vector` until p7-s3 (#243) added a third kind. Sidecars
  /// written before then carry only the first two spellings and load unchanged.
  ReferenceLayerKind kind = ReferenceLayerKind::Raster;

  bool visible = true;

  /// The scene's projection string when this layer was placed. Unlike the
  /// workspace box, a stale value here does not discard anything — the source
  /// file is still the truth, so the placement is simply re-derived.
  std::string framed_crs;
};

/// One scene's Layer-2 state. Every modeled field is optional so that ABSENT
/// is distinguishable from "present and equal to the default": a scene with no
/// stored render mode must fall back to the application default, not to
/// whatever `false` happens to mean.
struct SceneState {
  /// The version as PARSED (re-emitted on write, never silently downgraded or
  /// upgraded), so a file from a newer editor keeps announcing itself.
  int version = 1;

  std::optional<SceneViewState> view;

  /// Per-scene render mode: true = daytime Textured, false = flat Sober.
  /// Overrides the application default for this scene only.
  std::optional<bool> textured;

  /// The editing mode this scene was last in (p8-s2, #246): Map or Scenario.
  ///
  /// Layer 2 by ADR-0008 — it is which pane of the editor you were working in,
  /// not content. Absent means Map, which is what every scene written before
  /// p8-s2 means and what a plain `.xodr` wants.
  ///
  /// ★ THE ACTIVE SCENARIO FILE IS DELIBERATELY NOT STORED BESIDE IT. ADR-0014
  /// §9 stem-matches `town.xosc` to `town.xodr`, so the path is derivable and a
  /// stored copy could only ever disagree with it. That is the other half of
  /// the question the #246 review comment posed.
  std::optional<EditorMode> mode;

  /// The workspace box and the frame it was framed in (p7-s5, #324).
  std::optional<SceneWorkspaceState> workspace;

  /// Imported GIS reference layers (p7-s2, #242). Layer 2 by ADR-0008, which
  /// names "import metadata" as a resident of the native container — imagery
  /// and vectors are authoring reference and have no business in the `.xodr`.
  ///
  /// Absent and empty are distinguished as everywhere else here, but they mean
  /// the same thing to a reader; the optional exists so a sidecar that never
  /// had the block does not grow an empty array on rewrite.
  std::optional<std::vector<SceneReferenceLayer>> reference_layers;

  /// The WHOLE parsed root object, kept as the merge base for to_json() — this
  /// is what makes unknown keys (a `snap` or `session` block from a future
  /// build) survive a rewrite. Empty for a state that never came off disk.
  QJsonObject raw;
};

/// Reading and writing `<scene>.rmscene.json`.
///
/// Free functions rather than a class: unlike LibraryManifest there is no
/// state to own — SceneState IS the parsed document.
namespace scene_sidecar {

/// The schema version this build writes for a state it authored itself.
inline constexpr int kSupportedVersion = 1;

/// The sidecar file name suffix, appended to the scene's STEM (so
/// `town.xodr` pairs with `town.rmscene.json`, matching how the kernel names
/// its terrain sidecar `<stem>.terrain.asc`).
inline constexpr const char* kSuffix = ".rmscene.json";

/// The sidecar path for a scene file. Built by concatenation rather than
/// replace_extension so a bare relative `scene.xodr` (empty parent path)
/// yields `scene.rmscene.json` and not an absolute-looking path.
[[nodiscard]] std::filesystem::path path_for(const std::filesystem::path& scene);

/// Parses sidecar bytes. Errors only on things that make the document
/// meaningless: unparseable JSON, a non-object root, or a missing/non-integer
/// `scene_version`. Everything below that degrades — a malformed `view` is
/// dropped whole with one warning, unknown keys are retained.
[[nodiscard]] Expected<SceneState> parse(const QByteArray& json);

/// Reads and parses the sidecar at `path`. A missing file is
/// ErrorCode::FileNotFound — the single most common case (every pure `.xodr`),
/// so callers must not log it as a problem.
[[nodiscard]] Expected<SceneState> load(const std::filesystem::path& path);

/// Serializes `state`, MERGING over its retained `raw` root: the keys this
/// build owns are overwritten (or removed when their optional is empty) and
/// every other key is passed through untouched. Note this is the opposite of
/// LibraryManifest's verbatim-wins rule — here the modeled fields are the live
/// values being saved, and `raw` is only the forward-compat carrier.
[[nodiscard]] QByteArray to_json(const SceneState& state);

/// Writes `state` to `path` atomically (QSaveFile — the Project::create
/// pattern): the sidecar appears whole or not at all, so a crash mid-write
/// never leaves a half-parsed file behind.
[[nodiscard]] Expected<void> save(const std::filesystem::path& path, const SceneState& state);

} // namespace scene_sidecar

} // namespace roadmaker::editor
