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

#include "document/scene_sidecar.hpp"

#include <spdlog/spdlog.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>
#include <cmath>

namespace roadmaker::editor::scene_sidecar {

namespace {

constexpr auto kVersionKey = "scene_version";
constexpr auto kViewKey = "view";
constexpr auto kTexturedKey = "textured";
constexpr auto kWorkspaceKey = "workspace";
constexpr auto kReferenceLayersKey = "reference_layers";
constexpr auto kPathKey = "path";
constexpr auto kKindKey = "kind";
constexpr auto kVisibleKey = "visible";
constexpr auto kFramedCrsKey = "framed_crs";
constexpr auto kVectorKind = "vector";
constexpr auto kRasterKind = "raster";
constexpr auto kExtentsKey = "extents";
constexpr auto kCrsKey = "crs";
constexpr auto kTargetKey = "target";
constexpr auto kYawKey = "yaw";
constexpr auto kPitchKey = "pitch";
constexpr auto kDistanceKey = "distance";
constexpr auto kProjectionKey = "projection";
constexpr auto kPerspective = "perspective";
constexpr auto kOrthographic = "orthographic";

/// Camera state is float; JSON numbers are double. Writing the widened double
/// straight through round-trips exactly but prints all 17 of its digits
/// ("yaw": 0.800000011920929), and ADR-0008 sells the container as
/// git-friendly and diffable.
///
/// So: the SHORTEST decimal that still reloads the identical float. Nine
/// significant digits (FLT_DECIMAL_DIG) is the guaranteed identity width, so
/// the loop always terminates — but a fixed 9 is not the answer either, since
/// 9 digits of the WIDENED float is "0.800000012", not "0.8". Only the
/// round-trip check finds the short form.
///
/// The result is also a fixed point of write → parse → write (the emitted
/// double is strtod of the text, whose own shortest form is that same text),
/// which is what makes save → load → save byte-identical.
///
/// Significant digits, never decimal places: rounding 12345.6789 to six
/// PLACES is not a fixed point, because a float cannot hold that many digits
/// and the value shifts on the first pass. QString::number(double, ...) is
/// C-locale by definition — QLocale::toString would emit a comma under a
/// German locale and is the trap here.
[[nodiscard]] double to_json_float(float value) {
  const auto widened = static_cast<double>(value);
  for (int digits = 1; digits < 9; ++digits) {
    const double candidate = QString::number(widened, 'g', digits).toDouble();
    if (static_cast<float>(candidate) == value) {
      return candidate;
    }
  }
  return QString::number(widened, 'g', 9).toDouble();
}

/// The double twin of to_json_float, for values that are double all the way
/// down (p7-s5, #324): the workspace box is metres in the kernel frame, and a
/// scene sitting at UTM coordinates would lose a decimetre through a float.
///
/// Same rule, same reason, wider guaranteed width — 17 significant digits
/// (DBL_DECIMAL_DIG) is where the loop is bound to terminate. Kept as a
/// separate function rather than a template because the two differ in exactly
/// that bound, and a template would hide the one number that matters.
[[nodiscard]] double to_json_double(double value) {
  for (int digits = 1; digits < 17; ++digits) {
    const double candidate = QString::number(value, 'g', digits).toDouble();
    if (candidate == value) {
      return candidate;
    }
  }
  return QString::number(value, 'g', 17).toDouble();
}

/// A finite JSON number, or nullopt. Qt writes NaN/±Inf as `null` (RFC 4627),
/// which parses back as a non-double — so this rejects exactly what the writer
/// refuses to emit.
[[nodiscard]] std::optional<float> finite_float(const QJsonValue& value) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number)) {
    return std::nullopt;
  }
  return static_cast<float>(number);
}

[[nodiscard]] bool all_finite(const SceneViewState& view) {
  return std::isfinite(view.target[0]) && std::isfinite(view.target[1]) &&
         std::isfinite(view.target[2]) && std::isfinite(view.yaw) && std::isfinite(view.pitch) &&
         std::isfinite(view.distance);
}

/// Parses the `view` block. All-or-nothing on the KNOWN grammar (the house
/// rule for every rm: carrier): every modeled field is required and must be a
/// finite number, and a single malformed one drops the whole block with one
/// warning rather than restoring a half-camera. Unknown fields inside the
/// block are ignored here and preserved by to_json()'s merge.
[[nodiscard]] std::optional<SceneViewState> parse_view(const QJsonValue& value) {
  if (value.isUndefined()) {
    return std::nullopt; // absent is normal, not a problem — say nothing
  }
  if (!value.isObject()) {
    spdlog::warn("scene sidecar: 'view' is not an object — ignoring it");
    return std::nullopt;
  }
  const QJsonObject object = value.toObject();

  const QJsonValue target_value = object.value(QLatin1String(kTargetKey));
  if (!target_value.isArray() || target_value.toArray().size() != 3) {
    spdlog::warn("scene sidecar: 'view.target' is not an array of 3 numbers — ignoring the view");
    return std::nullopt;
  }
  SceneViewState view;
  const QJsonArray target = target_value.toArray();
  for (int index = 0; index < 3; ++index) {
    const auto component = finite_float(target.at(index));
    if (!component) {
      spdlog::warn("scene sidecar: 'view.target' holds a non-finite value — ignoring the view");
      return std::nullopt;
    }
    view.target[static_cast<std::size_t>(index)] = *component;
  }

  const auto yaw = finite_float(object.value(QLatin1String(kYawKey)));
  const auto pitch = finite_float(object.value(QLatin1String(kPitchKey)));
  const auto distance = finite_float(object.value(QLatin1String(kDistanceKey)));
  if (!yaw || !pitch || !distance) {
    spdlog::warn(
        "scene sidecar: 'view' is missing a finite yaw/pitch/distance — ignoring the view");
    return std::nullopt;
  }
  view.yaw = *yaw;
  view.pitch = *pitch;
  view.distance = *distance;

  const QString projection = object.value(QLatin1String(kProjectionKey)).toString();
  if (projection == QLatin1String(kPerspective)) {
    view.projection = ProjectionMode::Perspective;
  } else if (projection == QLatin1String(kOrthographic)) {
    view.projection = ProjectionMode::Orthographic;
  } else {
    spdlog::warn("scene sidecar: 'view.projection' is not perspective/orthographic — "
                 "ignoring the view");
    return std::nullopt;
  }
  return view;
}

/// Parses the `workspace` block (p7-s5, #324). Same all-or-nothing house rule
/// as `view`: `extents` must be four finite numbers, and `crs` must be a string
/// if present — absent means "framed in an unprojected scene", which is a
/// frame, not a missing field.
[[nodiscard]] std::optional<SceneWorkspaceState> parse_workspace(const QJsonValue& value) {
  if (value.isUndefined()) {
    return std::nullopt; // absent is normal
  }
  if (!value.isObject()) {
    spdlog::warn("scene sidecar: 'workspace' is not an object — ignoring it");
    return std::nullopt;
  }
  const QJsonObject object = value.toObject();

  const QJsonValue extents_value = object.value(QLatin1String(kExtentsKey));
  if (!extents_value.isArray() || extents_value.toArray().size() != 4) {
    spdlog::warn("scene sidecar: 'workspace.extents' is not an array of 4 numbers — "
                 "ignoring the workspace");
    return std::nullopt;
  }
  SceneWorkspaceState workspace;
  const QJsonArray extents = extents_value.toArray();
  for (int index = 0; index < 4; ++index) {
    const QJsonValue component = extents.at(index);
    if (!component.isDouble() || !std::isfinite(component.toDouble())) {
      spdlog::warn("scene sidecar: 'workspace.extents' holds a non-finite value — "
                   "ignoring the workspace");
      return std::nullopt;
    }
    workspace.extents[static_cast<std::size_t>(index)] = component.toDouble();
  }
  if (workspace.extents[0] > workspace.extents[2] || workspace.extents[1] > workspace.extents[3]) {
    spdlog::warn("scene sidecar: 'workspace.extents' is inverted — ignoring the workspace");
    return std::nullopt;
  }

  const QJsonValue crs = object.value(QLatin1String(kCrsKey));
  if (crs.isString()) {
    workspace.crs = crs.toString().toStdString();
  } else if (!crs.isUndefined() && !crs.isNull()) {
    spdlog::warn("scene sidecar: 'workspace.crs' is not a string — ignoring the workspace");
    return std::nullopt;
  }
  return workspace;
}

/// Parses the `reference_layers` array (p7-s2, #242).
///
/// Per-entry rather than all-or-nothing, unlike `view` and `workspace`: those
/// are one indivisible thing each, while this is a list, and dropping five good
/// layers because a sixth is malformed helps nobody. An entry with no usable
/// `path` is the only thing skipped, because that is the one field nothing can
/// be recomputed without.
[[nodiscard]] std::optional<std::vector<SceneReferenceLayer>>
parse_reference_layers(const QJsonValue& value) {
  if (value.isUndefined() || value.isNull()) {
    return std::nullopt;
  }
  if (!value.isArray()) {
    spdlog::warn("scene sidecar: 'reference_layers' is not an array — ignoring it");
    return std::nullopt;
  }

  std::vector<SceneReferenceLayer> layers;
  const QJsonArray array = value.toArray();
  for (const QJsonValue& entry : array) {
    if (!entry.isObject()) {
      spdlog::warn("scene sidecar: a 'reference_layers' entry is not an object — skipping it");
      continue;
    }
    const QJsonObject object = entry.toObject();
    const QJsonValue path = object.value(QLatin1String(kPathKey));
    if (!path.isString() || path.toString().isEmpty()) {
      spdlog::warn("scene sidecar: a 'reference_layers' entry has no 'path' — skipping it");
      continue;
    }

    SceneReferenceLayer layer;
    layer.path = path.toString().toStdString();

    const QJsonValue kind = object.value(QLatin1String(kKindKey));
    if (kind.isString()) {
      layer.vector = kind.toString() == QLatin1String(kVectorKind);
      if (!layer.vector && kind.toString() != QLatin1String(kRasterKind)) {
        spdlog::warn("scene sidecar: reference layer '{}' has an unknown kind '{}' — reading it "
                     "as a raster",
                     layer.path,
                     kind.toString().toStdString());
      }
    }

    const QJsonValue visible = object.value(QLatin1String(kVisibleKey));
    if (visible.isBool()) {
      layer.visible = visible.toBool();
    }

    const QJsonValue framed = object.value(QLatin1String(kFramedCrsKey));
    if (framed.isString()) {
      layer.framed_crs = framed.toString().toStdString();
    }

    layers.push_back(std::move(layer));
  }
  return layers;
}

/// The `reference_layers` array to write.
[[nodiscard]] QJsonArray reference_layers_array(const std::vector<SceneReferenceLayer>& layers) {
  QJsonArray array;
  for (const SceneReferenceLayer& layer : layers) {
    QJsonObject object;
    object.insert(QLatin1String(kPathKey), QString::fromStdString(layer.path));
    object.insert(QLatin1String(kKindKey), QLatin1String(layer.vector ? kVectorKind : kRasterKind));
    object.insert(QLatin1String(kVisibleKey), layer.visible);
    object.insert(QLatin1String(kFramedCrsKey), QString::fromStdString(layer.framed_crs));
    array.push_back(object);
  }
  return array;
}

/// The `workspace` block to write, merged over whatever was parsed.
[[nodiscard]] QJsonObject workspace_object(const SceneWorkspaceState& workspace, QJsonObject base) {
  QJsonArray extents;
  for (const double component : workspace.extents) {
    extents.push_back(to_json_double(component));
  }
  base.insert(QLatin1String(kExtentsKey), extents);
  base.insert(QLatin1String(kCrsKey), QString::fromStdString(workspace.crs));
  return base;
}

/// The `view` block to write, merged over whatever was parsed so a field this
/// build does not model (a future `view.roll`) survives the rewrite.
[[nodiscard]] QJsonObject view_object(const SceneViewState& view, QJsonObject base) {
  QJsonArray target;
  for (const float component : view.target) {
    target.push_back(to_json_float(component));
  }
  base.insert(QLatin1String(kTargetKey), target);
  base.insert(QLatin1String(kYawKey), to_json_float(view.yaw));
  base.insert(QLatin1String(kPitchKey), to_json_float(view.pitch));
  base.insert(QLatin1String(kDistanceKey), to_json_float(view.distance));
  base.insert(QLatin1String(kProjectionKey),
              QLatin1String(view.projection == ProjectionMode::Orthographic ? kOrthographic
                                                                            : kPerspective));
  return base;
}

} // namespace

std::filesystem::path path_for(const std::filesystem::path& scene) {
  return scene.parent_path() / (scene.stem().string() + kSuffix);
}

Expected<SceneState> parse(const QByteArray& json) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError) {
    return make_error(ErrorCode::InvalidArgument,
                      "scene sidecar is not valid JSON: " + error.errorString().toStdString());
  }
  if (!document.isObject()) {
    return make_error(ErrorCode::InvalidArgument, "scene sidecar root is not an object");
  }
  const QJsonObject root = document.object();

  const QJsonValue version = root.value(QLatin1String(kVersionKey));
  if (!version.isDouble()) {
    return make_error(ErrorCode::InvalidArgument,
                      "scene sidecar is missing an integer scene_version");
  }

  SceneState state;
  state.version = version.toInt();
  // Forward compatibility: a newer schema still parses the fields this build
  // knows, and the merge in to_json() carries the rest through untouched.
  if (state.version > kSupportedVersion) {
    spdlog::warn("scene sidecar version {} is newer than supported version {} — "
                 "parsing known fields only",
                 state.version,
                 kSupportedVersion);
  }
  state.view = parse_view(root.value(QLatin1String(kViewKey)));
  const QJsonValue textured = root.value(QLatin1String(kTexturedKey));
  if (textured.isBool()) {
    state.textured = textured.toBool();
  } else if (!textured.isUndefined()) {
    spdlog::warn("scene sidecar: 'textured' is not a boolean — falling back to the app default");
  }
  state.workspace = parse_workspace(root.value(QLatin1String(kWorkspaceKey)));
  state.reference_layers = parse_reference_layers(root.value(QLatin1String(kReferenceLayersKey)));
  // The WHOLE root, not the leftovers: to_json() merges the owned keys over it,
  // which is both simpler than tracking which keys were unknown and provably
  // byte-stable (QJsonObject iterates key-sorted, so insertion order cannot
  // change the output).
  state.raw = root;
  return state;
}

Expected<SceneState> load(const std::filesystem::path& path) {
  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return make_error(ErrorCode::FileNotFound, "cannot read scene sidecar", path.string());
  }
  auto state = parse(file.readAll());
  if (!state) {
    return make_error(state.error().code, state.error().message, path.string());
  }
  return state;
}

QByteArray to_json(const SceneState& state) {
  QJsonObject root = state.raw;
  // The PARSED version, like LibraryManifest — a file written by a newer build
  // keeps announcing itself rather than being silently downgraded by this one.
  root.insert(QLatin1String(kVersionKey), state.version);
  if (state.view && all_finite(*state.view)) {
    root.insert(QLatin1String(kViewKey),
                view_object(*state.view, root.value(QLatin1String(kViewKey)).toObject()));
  } else {
    if (state.view) {
      // Qt serializes a non-finite double as `null`, which would not parse back
      // — omit the block instead, so the writer never emits what the reader
      // refuses (the same rule the rm: carriers follow).
      spdlog::warn("scene sidecar: refusing to write a non-finite camera pose");
    }
    root.remove(QLatin1String(kViewKey));
  }
  if (state.textured) {
    root.insert(QLatin1String(kTexturedKey), *state.textured);
  } else {
    root.remove(QLatin1String(kTexturedKey));
  }
  if (state.workspace) {
    root.insert(
        QLatin1String(kWorkspaceKey),
        workspace_object(*state.workspace, root.value(QLatin1String(kWorkspaceKey)).toObject()));
  } else {
    root.remove(QLatin1String(kWorkspaceKey));
  }
  if (state.reference_layers) {
    root.insert(QLatin1String(kReferenceLayersKey),
                reference_layers_array(*state.reference_layers));
  } else {
    root.remove(QLatin1String(kReferenceLayersKey));
  }
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

Expected<void> save(const std::filesystem::path& path, const SceneState& state) {
  // QSaveFile: the sidecar appears atomically or not at all (Project::create
  // pattern) — a half-written file would be the one thing that CAN cost the
  // user their view state.
  QSaveFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::WriteOnly)) {
    return make_error(ErrorCode::IoFailure, "cannot open scene sidecar for writing", path.string());
  }
  file.write(to_json(state));
  if (!file.commit()) {
    return make_error(ErrorCode::IoFailure, "cannot commit scene sidecar", path.string());
  }
  return {};
}

} // namespace roadmaker::editor::scene_sidecar
