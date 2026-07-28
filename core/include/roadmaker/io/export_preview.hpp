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

/// What an export WOULD produce, computed without writing anything (p7-s1,
/// #241).
///
/// Two previews, and they are built on opposite principles because the two
/// export paths differ:
///
///   - `preview_xodr_export` runs the REAL writer in memory (`write_xodr`
///     already returns a std::string) and counts its OUTPUT. A summary derived
///     from the bytes that would be written cannot drift from them, so this
///     half needs no anti-drift gate at all.
///   - `preview_mesh_export` cannot do that: the mesh exporters are path-only,
///     and "preview without writing files" forbids a temp-file round trip. So
///     it is a MANIFEST that re-states the exporters' policy, and it lives
///     beside them in core/src/io (where their private material vocabulary
///     lives) with a reconciliation gate in core/tests/test_export_preview.cpp
///     that exports for real and compares. A manifest that is a second,
///     unreconciled implementation of the export is a lie waiting to happen.

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/xodr/diagnostic.hpp"
#include "roadmaker/xodr/rm_codes.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {

/// Which mesh exporter a preview describes.
///
/// This is a parameter, not a decoration: the two formats put DIFFERENT
/// geometry in the file. glTF caches one shared mesh per prop model and emits
/// a node per instance (gltf_exporter.cpp, prop_mesh_for); USD bakes
/// world-space geometry for every instance (usd_exporter.cpp, bake_instance).
/// For two hundred trees the .glb stores one tree and the .usda stores two
/// hundred, so a format-agnostic triangle count would be wrong in one of them.
enum class MeshExportFormat {
  Gltf,
  Usd,
};

/// One channel of NetworkMesh (mesh.hpp), plus SignalFaces, which is not a
/// member of its own — it rides on `signal_instances` and exports only to
/// glTF.
///
/// The switch over this enum in export_preview.cpp carries no `default:`, so
/// adding a value is a compile error until every format states whether it
/// walks the channel. That, plus the totality gate in the tests, is what stops
/// a future channel repeating the silent omission #390 records.
enum class MeshChannel {
  Roads,
  JunctionFloors,
  Surfaces,
  Terrain,
  Bridges,
  Objects,
  SignalInstances,
  SignalFaces,
};

/// Number of values in MeshChannel — iterate [0, kMeshChannelCount).
inline constexpr std::size_t kMeshChannelCount = 8;

/// Why a channel, or part of one, does not reach the file.
enum class OmissionReason {
  None,              ///< fully exported
  ChannelEmpty,      ///< nothing in the mesh to export
  ChannelNotWalked,  ///< the exporter never visits it (surfaces/terrain — #390)
  FormatUnsupported, ///< the format cannot carry it (USD sign-face textures — #364)
  ModelNotFound,     ///< an instance names a prop/signal model that does not resolve
};

/// One channel as the chosen exporter treats it.
///
/// COUNTING RULE — `vertices` and `triangles` are FILE-WEIGHTED: they are what
/// the written file stores, not what a renderer would draw. So a glTF scene
/// with two hundred identical trees reports one tree's geometry (the shared
/// mesh) while the same scene in USD reports two hundred (each baked). That
/// asymmetry is the honest answer to "what will be in the file", it is what
/// the reconciliation gate can actually check against a reloaded document, and
/// it is why `elements`/`exported_elements` are reported separately — those
/// are the instance counts.
struct MeshChannelPreview {
  MeshChannel channel{};

  /// Stable machine-readable id — "roads", "junction_floors", "surfaces",
  /// "terrain", "bridges", "objects", "signal_instances", "signal_faces".
  /// Never translated; the editor maps it to display text.
  std::string_view label;

  std::size_t elements = 0;          ///< records present in the mesh
  std::size_t exported_elements = 0; ///< records the file would carry
  std::size_t vertices = 0;          ///< vertices stored IN THE FILE
  std::size_t triangles = 0;         ///< triangles stored IN THE FILE

  OmissionReason reason = OmissionReason::None;

  /// English prose explaining `reason`, naming the owning issue where one
  /// exists. Kernel-authored UI copy, like Diagnostic::message — consumers
  /// render it raw. Tests assert on `reason`, never on this.
  std::string detail;
};

/// One material as the exporter would name and parameterise it. The names and
/// colours come from the exporters' own shared vocabulary
/// (core/src/io/mesh_export_common.hpp), never from a second palette.
struct MaterialPreview {
  std::string name;              ///< the exporter's own material name
  std::array<double, 4> color{}; ///< linear RGBA base colour, as written
  double roughness = 0.0;
  std::size_t triangles = 0; ///< triangles bound to it, file-weighted
  bool textured = false;     ///< carries an embedded image (glTF sign faces)
};

/// Axis-aligned bounds in the EXPORT frame (right-handed Y-up, meters) — the
/// frame the file is written in (io_common::to_export_frame). Deliberately not
/// the kernel frame: reporting kernel coordinates would make reconciliation
/// against a reloaded document impossible without an inverse rotation, i.e. a
/// second place for the boundary convention to drift. Consumers that show
/// these must say "export frame", or a user comparing them against the
/// viewport is misled.
struct ExportBounds {
  std::array<double, 3> min{};
  std::array<double, 3> max{};
  bool valid = false;
};

/// What export_glb / export_usda would write, without writing it.
struct ScenePreview {
  MeshExportFormat format{};

  /// Whether THIS kernel build can actually write the format. False for Usd
  /// when the kernel was built without RM_BUILD_USD. The manifest is computed
  /// and correct either way — it depends on export policy, not on tinyusdz.
  bool available = false;

  /// The empty-mesh guard, previewed rather than hit at a file dialog.
  bool would_export = false;

  /// Exactly what the exporter would return when `would_export` is false.
  std::optional<Error> refusal;

  /// Always kMeshChannelCount rows, in MeshChannel order — including the
  /// empty and the omitted ones. A channel that is silently absent from a
  /// report is how #390 stayed invisible for two pillars.
  std::vector<MeshChannelPreview> channels;

  /// Sorted by name, so the report is deterministic.
  std::vector<MaterialPreview> materials;

  std::size_t total_vertices = 0;
  std::size_t total_triangles = 0;

  /// File structure, for reconciliation against a reloaded document.
  std::size_t mesh_count = 0;
  std::size_t node_count = 0;
  std::size_t image_count = 0;

  ExportBounds bounds;

  /// Advisories about the export — omissions, format limits, unresolved
  /// models. Diagnostic-shaped so the editor renders them in the same table
  /// as validator findings.
  std::vector<Diagnostic> notes;
};

/// What `export_glb`/`export_usda` WOULD write for `mesh`, without writing
/// anything.
///
/// Pure: never touches the filesystem, never mutates `mesh`, and returns the
/// same value for the same input. Defined for BOTH formats regardless of
/// RM_BUILD_USD — consult `ScenePreview::available` for whether this build can
/// write the result.
[[nodiscard]] RM_API ScenePreview preview_mesh_export(const NetworkMesh& mesh,
                                                      MeshExportFormat format);

/// Whether THIS kernel build can write `format`.
///
/// Defined in the .cpp on purpose. RM_HAVE_USD is a PUBLIC compile definition
/// on the core target, so an inline reader would evaluate in the CALLER's
/// translation unit and could disagree with a differently-configured shared
/// kernel it is linked against.
[[nodiscard]] RM_API bool mesh_export_available(MeshExportFormat format);

// ---------------------------------------------------------------- OpenDRIVE

/// One Layer-1 `rm:` extension record type, counted in the output.
struct XodrRecordPreview {
  std::string code; ///< always satisfies is_registered_rm_code()
  RmCodeScope scope{};
  std::size_t count = 0;
};

/// What save_xodr would write, without writing it.
///
/// Every Layer-0 and Layer-1 number below is counted out of `xml` — the bytes
/// the writer actually produced — never re-walked from the RoadNetwork.
/// Re-walking would be a second implementation of the writer's emission
/// conditions and would need its own drift gate; counting the output cannot
/// disagree with the output.
struct XodrPreview {
  XodrVersion target_version{};

  bool would_write = false;

  /// write_xodr's refusal. NOTE the asymmetry this preview exists to correct:
  /// write_xodr does not call validate_network — it calls a separate,
  /// hard-failing validate() that stops at the FIRST defect and collapses it
  /// into one Error, discarding the rule id and every other finding. So this
  /// field is one message where `diagnostics` may hold twenty.
  std::optional<Error> refusal;

  /// Exactly write_xodr's bytes; empty when refused.
  std::string xml;
  std::size_t byte_count = 0;

  // --- Layer 0 (pure ASAM), counted out of `xml` -------------------------
  std::size_t road_count = 0;
  std::size_t junction_count = 0;
  std::size_t lane_section_count = 0;
  std::size_t lane_count = 0;
  std::size_t geometry_record_count = 0;
  std::size_t object_count = 0;
  std::size_t signal_count = 0;
  std::size_t controller_count = 0;

  /// Sum of every <road @length>, meters.
  double total_reference_length = 0.0;

  // --- Layer 1 (rm: userData, ADR-0008), counted out of `xml` ------------
  /// In kRmCodes order; only codes actually present.
  std::vector<XodrRecordPreview> rm_records;

  /// Foreign (non-rm:) userData codes preserved verbatim by the writer.
  std::vector<std::string> foreign_user_data_codes;

  /// The terrain .asc sidecar save_xodr would write BESIDE the .xodr — which
  /// write_xodr alone knows nothing about. Read back out of the emitted
  /// <userData code="rm:terrain"> value rather than recomputed, so the preview
  /// and the writer can never name different files. Empty when the scene
  /// carries no height field.
  std::string terrain_sidecar;

  /// validate_network's FULL advisory sweep, run BEFORE write_xodr so a
  /// refused write is explained by every finding rather than by the single
  /// message `refusal` collapses to.
  std::vector<Diagnostic> diagnostics;
};

/// What `save_xodr` WOULD write for `network`, without writing anything.
///
/// Layers 0 and 1 only (ADR-0008). The Layer-2 `.rmscene.json` companion is
/// editor state and never appears here.
[[nodiscard]] RM_API XodrPreview preview_xodr_export(const RoadNetwork& network,
                                                     std::string_view document_name = "roadmaker",
                                                     const WriterOptions& options = {});

} // namespace roadmaker
