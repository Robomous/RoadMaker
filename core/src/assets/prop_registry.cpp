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

// The public prop catalogue: the bundled models from prop_meshes.gen.cpp, with
// the open project's imported models layered over them (p6-s8, #322).
//
// WHY THE OVERLAY IS PROCESS-WIDE STATE. `props::model()` is reached from the
// mesh builder, the scene builder's instanced batching, prop placement, and both
// exporters — none of which has a project to hand, and several of which are deep
// inside kernel code that must not learn what a project is. ADR-0013 records the
// decision: the overlay lives here, behind the signature every one of those
// callers already uses, so not one of them changes. The cost is hidden state,
// and it is paid down by the two invariants this file exists to hold:
//
//   1. `register_project_models` REPLACES wholesale — a project switch can never
//      leave the previous project's assets resolvable.
//   2. `clear_project_models` restores the bundled catalogue EXACTLY.
//
// Both are tested (test_prop_library.cpp), because "the overlay is cleared" is
// the kind of claim that is true until the day it silently is not.
//
// THREADING. Registration is main-thread work performed before any mesh or scene
// build, which is what makes the unsynchronised read in `model()` correct. There
// is no lock here on purpose: adding one would imply a concurrency contract the
// rest of the prop path does not have.

#include "roadmaker/assets/prop_library.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roadmaker::props {

namespace {

/// The project overlay. Models are held by unique_ptr so that growing the vector
/// never moves a PropModel a caller is holding a pointer into — the pointers only
/// ever die when the overlay is replaced or cleared, which is exactly the
/// lifetime prop_library.hpp documents.
struct ProjectOverlay {
  std::vector<std::unique_ptr<PropModel>> models;
  /// Bundled ids followed by project ids. Rebuilt on every registration so
  /// `ids()` can hand out a reference without recomputing per call.
  std::vector<std::string> merged_ids;
};

ProjectOverlay& overlay() {
  static ProjectOverlay state;
  return state;
}

/// The project model for `id`, or nullptr. Linear, like the bundled lookup it
/// sits in front of — #510 owns measuring whether that matters once a project
/// can hold hundreds of assets.
const PropModel* project_model(std::string_view id) {
  for (const std::unique_ptr<PropModel>& model : overlay().models) {
    if (model->id == id) {
      return model.get();
    }
  }
  return nullptr;
}

void rebuild_merged_ids() {
  ProjectOverlay& state = overlay();
  state.merged_ids.clear();
  if (state.models.empty()) {
    return;
  }
  const std::vector<std::string>& builtin = detail::builtin_ids();
  state.merged_ids.reserve(builtin.size() + state.models.size());
  state.merged_ids = builtin;
  for (const std::unique_ptr<PropModel>& model : state.models) {
    // A project id that shadows a bundled one must not appear twice — `model()`
    // resolves it to the project's copy, so listing it once is the honest
    // answer to "what models are there".
    bool shadowed = false;
    for (const std::string& existing : state.merged_ids) {
      if (existing == model->id) {
        shadowed = true;
        break;
      }
    }
    if (!shadowed) {
      state.merged_ids.push_back(model->id);
    }
  }
}

} // namespace

const std::vector<std::string>& ids() {
  const ProjectOverlay& state = overlay();
  // ★ THIS GUARD, NOT THE CLEAR IN clear_project_models(), IS WHAT MAKES THE
  // "restores the bundled catalogue exactly" invariant hold. With no project
  // loaded the bundled list is returned by reference, so `merged_ids` is never
  // read and its contents cannot matter. (Found by sabotage: emptying
  // `merged_ids` on clear turns out to be unobservable, while deleting this
  // early return breaks the catalogue outright.) It also means the
  // overwhelmingly common case pays nothing for the overlay existing.
  if (state.models.empty()) {
    return detail::builtin_ids();
  }
  return state.merged_ids;
}

const PropModel* model(std::string_view id) {
  if (const PropModel* imported = project_model(id); imported != nullptr) {
    return imported;
  }
  return detail::builtin_model(id);
}

void register_project_models(std::vector<PropModel> models) {
  ProjectOverlay& state = overlay();
  state.models.clear();
  state.models.reserve(models.size());
  for (PropModel& model : models) {
    state.models.push_back(std::make_unique<PropModel>(std::move(model)));
  }
  rebuild_merged_ids();
}

void clear_project_models() {
  ProjectOverlay& state = overlay();
  state.models.clear();
  // Hygiene, not correctness: `ids()` stops reading `merged_ids` the moment the
  // overlay is empty (see the note there). Released anyway so a closed project's
  // id strings do not sit in memory for the rest of the session.
  state.merged_ids.clear();
}

bool is_project_model(std::string_view id) {
  return project_model(id) != nullptr;
}

} // namespace roadmaker::props
