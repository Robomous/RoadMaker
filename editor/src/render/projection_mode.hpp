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

// The projection mode lives in its own leaf header (fmt-s1, #325) because two
// layers need it and neither may include the other: viewport/camera.hpp owns
// the camera that projects, and document/scene_sidecar.hpp persists the mode
// as Layer-2 scene state. `document/` must never include `viewport/` — the
// established edge runs the other way (viewport/framing.hpp includes
// document/selection_model.hpp) — so the shared datum sits in `render/`, the
// Qt-free, GL-free leaf that already holds CameraMatrices.

namespace roadmaker::editor {

/// How the camera projects (GW-1 step 11). Both modes share the orbit state —
/// see OrbitCamera::matrices() for why the O/P toggle cannot jump.
enum class ProjectionMode {
  Perspective,
  Orthographic,
};

} // namespace roadmaker::editor
