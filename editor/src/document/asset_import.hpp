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

// Bringing a user's own file into a project as a Library asset (p6-s8, #322).
//
// HEADLESS ON PURPOSE: no widget, no dialog, no MainWindow. Everything here is a
// pure function over a project directory and a source path, so the whole import
// — slug collision, the copy, the thumbnail, the manifest entry — is testable
// offscreen with a temp project. The dialog (AssetImportDialog) only collects the
// name, category and licence attestation and hands them here.
//
// AN IMPORT COPIES, per ADR-0013. The source file is copied into
// `<project>/assets/textures/` or `<project>/assets/props/`, and the manifest
// entry records the original absolute path as provenance. Referencing it
// where the user keeps it would fail "the asset survives close and reopen" the
// first time they tidy their Downloads folder.

#include "roadmaker/error.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <QString>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "document/library_manifest.hpp"

namespace roadmaker::editor {

/// What kind of asset a file becomes. Deliberately narrower than
/// `ProjectFilesModel::FileType`: that classifies everything on disk for a glyph,
/// this answers only "can it be imported, and as what".
enum class AssetImportKind : std::uint8_t {
  Material, ///< a Qt-decodable image -> a PBR-lite material definition
  Prop,     ///< a .glb / .gltf -> a prop model (props::import_prop_model)
};

/// What the user chose in the dialog, plus what the caller derived.
struct AssetImportRequest {
  std::filesystem::path source; ///< the file the user picked, absolute
  QString label;                ///< display name; the slug is derived from it
  QString category;             ///< Library grouping ("Materials", "Props", …)
  QString license;              ///< the user's attestation, free text; may be empty
};

/// What an import produced, for the caller to report.
struct AssetImportResult {
  QString key;                     ///< the catalogue key the asset is now under
  QString slug;                    ///< the derived, unique asset slug
  std::filesystem::path copied_to; ///< where the file now lives inside the project
  /// True when the requested slug was already taken and a suffix was added, so
  /// the caller can say which name the asset actually got.
  bool renamed = false;
};

/// The kind `path` would import as, or nullopt when nothing here accepts it.
///
/// Exists so a file dialog's filter, the drag-and-drop accept check and the
/// importer cannot disagree about what is importable — the same reason
/// `props::is_prop_model_extension` and `gis::is_vector_extension` exist.
[[nodiscard]] std::optional<AssetImportKind> asset_import_kind(const std::filesystem::path& path);

/// The filter string for a file dialog, built from the same lists
/// `asset_import_kind` consults.
[[nodiscard]] QString asset_import_filter();

/// A filesystem- and manifest-safe slug from a display label, e.g. "Worn Asphalt"
/// -> "worn_asphalt". Empty when the label has no usable characters at all.
[[nodiscard]] QString asset_slug(const QString& label);

/// Imports an image as a project material: copies it into
/// `<project>/assets/textures/`, writes a 96x96 thumbnail beside the manifest,
/// and returns the definition to upsert. The caller owns saving the manifest, so
/// one gesture can import and commit atomically.
///
/// ★ NEVER OVERWRITES AN EXISTING SLUG. A colliding name gets a numeric suffix
/// and `renamed` is set. That keeps every imported file at a unique path, which
/// also sidesteps the permanent negative caching in
/// `ViewportWidget::texture_for` — a re-import over the same path would other-
/// wise be stuck on a cached miss for the rest of the session.
[[nodiscard]] Expected<std::pair<LibraryMaterial, AssetImportResult>>
import_material_asset(const std::filesystem::path& project_dir, const AssetImportRequest& request);

/// Imports a glTF/GLB as a project prop: copies it into `<project>/assets/props/`,
/// reads it with `props::import_prop_model` to prove it is usable, and returns the
/// catalogue item to upsert plus the reader's diagnostics for the caller to report.
///
/// The item's `model` field is the slug, which becomes the OpenDRIVE
/// `<object @name>` of every placed instance — the only link between an object and
/// its model. The same no-overwrite rule as materials applies, and for a stronger
/// reason here: overwriting a slug already placed in a scene would silently swap
/// the geometry under it.
///
/// NO THUMBNAIL is written. #322 puts render-to-thumbnail out of scope and the
/// Library already falls back to a themed glyph for an item without one (#509).
[[nodiscard]] Expected<std::tuple<LibraryItem, AssetImportResult, std::vector<Diagnostic>>>
import_prop_asset(const std::filesystem::path& project_dir, const AssetImportRequest& request);

/// Reads back every project-backed prop model a manifest declares and hands them
/// to `props::register_project_models`, so an imported prop resolves for the mesh
/// builder, the renderer and both exporters exactly as a bundled one does.
///
/// A model file that has gone missing is reported and SKIPPED — the project still
/// opens. Returns the diagnostics for the caller to route to the panel.
[[nodiscard]] std::vector<Diagnostic>
register_project_prop_models(const std::filesystem::path& project_dir,
                             const LibraryManifest& manifest);

} // namespace roadmaker::editor
