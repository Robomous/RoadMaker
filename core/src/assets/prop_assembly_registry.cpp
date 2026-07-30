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

// The public assembly catalogue (p6-s9, #323): the bundled composite props, with
// the open project's own definitions layered over them.
//
// The structure is deliberately the SAME SHAPE as prop_registry.cpp, down to the
// early return in `assembly_ids()` — an assembly is another asset resolved by id
// from inside code that must not learn what a project is (`signalize_junction`
// resolves a mount id in the kernel, with no project to hand), so it gets the same
// process-wide overlay behind the same unchanged signature, and pays it down with
// the same two invariants:
//
//   1. `register_project_assemblies` REPLACES wholesale.
//   2. `clear_project_assemblies` restores the bundled catalogue EXACTLY.
//
// WHY THE BUNDLED TABLE IS HAND-WRITTEN AND NOT GENERATED. gen_prop_meshes.py
// emits geometry; an assembly contains no geometry at all — it is a list of model
// ids and offsets. Those offsets ARE the §1.5 clearance numbers, so they belong
// where they can cite `roadmaker::defaults` directly, which a stdlib-only
// generator script cannot do.

#include "roadmaker/assets/prop_assembly.hpp"
#include "roadmaker/road/defaults.hpp"

#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roadmaker::props {

namespace {

/// The bundled mast-arm traffic signal — the demo assembly, and the shape every
/// other assembly follows.
///
/// Its numbers are §1.5's, not invented here: the heads' housing bottoms sit at
/// the default mast-arm clearance, the arm's underside sits one housing height
/// above that so the heads hang flush under it, and the arm reaches two arterial
/// lane widths with one head over each lane's centre.
///
/// ★ THE ARM'S QUARTER-TURN IS WHAT MAKES IT AN ARM. Every model in the prop
/// catalogue faces +x at local heading 0, and `mast_arm` is authored lying along
/// +x — so without `dyaw` it would reach along the ROAD instead of across it.
///
/// The arm reaches toward +t, which is the LEFT of the reference line: dropped on
/// the right-hand side of a road, the arm correctly overhangs the carriageway.
/// Dropped on the left, the user turns the whole assembly with the rotation ring,
/// exactly as they would aim a sign.
std::vector<PropAssembly> make_builtin() {
  const double arm_underside = defaults::kSignalClearance + defaults::kSignalHousingHeight;
  const double lane = defaults::kArterialLaneWidth;
  const double quarter_turn = std::numbers::pi / 2.0;

  PropAssembly signal_mast;
  signal_mast.id = "signal_mast";
  signal_mast.label = "Traffic signal, mast arm";
  signal_mast.parts = {
      AssemblyPart{.model = "pole_signal"},
      AssemblyPart{.model = "mast_arm", .dz = arm_underside, .dyaw = quarter_turn},
      AssemblyPart{.model = "signal_head", .dv = lane * 0.5, .dz = defaults::kSignalClearance},
      AssemblyPart{.model = "signal_head", .dv = lane * 1.5, .dz = defaults::kSignalClearance},
  };

  return {std::move(signal_mast)};
}

const std::vector<PropAssembly>& builtin_table() {
  static const std::vector<PropAssembly> table = make_builtin();
  return table;
}

/// The project overlay. Held by unique_ptr for the reason prop_registry.cpp spells
/// out: growing the vector must never move a PropAssembly a caller holds a pointer
/// into. The pointers die only on replace or clear.
struct ProjectOverlay {
  std::vector<std::unique_ptr<PropAssembly>> assemblies;
  /// Bundled ids followed by project ids, rebuilt on every registration so
  /// `assembly_ids()` can hand out a reference without recomputing per call.
  std::vector<std::string> merged_ids;
};

ProjectOverlay& overlay() {
  static ProjectOverlay state;
  return state;
}

const PropAssembly* project_assembly(std::string_view id) {
  for (const std::unique_ptr<PropAssembly>& candidate : overlay().assemblies) {
    if (candidate->id == id) {
      return candidate.get();
    }
  }
  return nullptr;
}

void rebuild_merged_ids() {
  ProjectOverlay& state = overlay();
  state.merged_ids.clear();
  if (state.assemblies.empty()) {
    return;
  }
  const std::vector<std::string>& builtin = detail::builtin_assembly_ids();
  state.merged_ids.reserve(builtin.size() + state.assemblies.size());
  state.merged_ids = builtin;
  for (const std::unique_ptr<PropAssembly>& candidate : state.assemblies) {
    // A project id that shadows a bundled one must appear once: `assembly()`
    // resolves it to the project's copy, so listing it twice would be a lie about
    // how many assemblies there are.
    bool shadowed = false;
    for (const std::string& existing : state.merged_ids) {
      if (existing == candidate->id) {
        shadowed = true;
        break;
      }
    }
    if (!shadowed) {
      state.merged_ids.push_back(candidate->id);
    }
  }
}

} // namespace

namespace detail {

const std::vector<std::string>& builtin_assembly_ids() {
  static const std::vector<std::string> table = [] {
    std::vector<std::string> out;
    out.reserve(builtin_table().size());
    for (const PropAssembly& entry : builtin_table()) {
      out.push_back(entry.id);
    }
    return out;
  }();
  return table;
}

const PropAssembly* builtin_assembly(std::string_view id) {
  for (const PropAssembly& entry : builtin_table()) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace detail

const std::vector<std::string>& assembly_ids() {
  const ProjectOverlay& state = overlay();
  // The same guard prop_registry.cpp calls out: with no project loaded the bundled
  // list is returned by reference, so `merged_ids` is never read and the "clearing
  // restores the catalogue exactly" invariant holds by construction rather than by
  // remembering to clear.
  if (state.assemblies.empty()) {
    return detail::builtin_assembly_ids();
  }
  return state.merged_ids;
}

const PropAssembly* assembly(std::string_view id) {
  if (const PropAssembly* defined = project_assembly(id); defined != nullptr) {
    return defined;
  }
  return detail::builtin_assembly(id);
}

void register_project_assemblies(std::vector<PropAssembly> assemblies) {
  ProjectOverlay& state = overlay();
  state.assemblies.clear();
  state.assemblies.reserve(assemblies.size());
  for (PropAssembly& entry : assemblies) {
    state.assemblies.push_back(std::make_unique<PropAssembly>(std::move(entry)));
  }
  rebuild_merged_ids();
}

void clear_project_assemblies() {
  ProjectOverlay& state = overlay();
  state.assemblies.clear();
  state.merged_ids.clear();
}

bool is_project_assembly(std::string_view id) {
  return project_assembly(id) != nullptr;
}

} // namespace roadmaker::props
