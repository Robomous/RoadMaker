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

#include "roadmaker/export.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// Composite prop assets — "assemblies" (p6-s9, #323). An assembly is a handful
/// of bundled prop models pinned together by fixed relative transforms: a mast-arm
/// traffic signal is a pole, a horizontal arm, and two heads, placed as one unit,
/// moved as one unit, deleted as one unit.
///
/// ★ NOT to be confused with `roadmaker::edit::assembly` — that namespace builds
/// parametric T/X road JUNCTIONS and predates this by a long way. The Library
/// keeps them apart too: category "Assemblies" is junctions, "Prop assemblies" is
/// this. #323's own issue text conflated them; see the correction comment there.
///
/// ★ NOT a Prop Set either. A Prop Set (p6-s5, #239) is a weighted RANDOM
/// SCATTER — one model is drawn per placed instance. An assembly is RIGID — every
/// part is placed, every time, at its authored offset.
namespace roadmaker::props {

/// One part of an assembly: a bundled prop model plus its pose in the assembly's
/// local frame.
///
/// THE LOCAL FRAME is the owning road's frame at the assembly's anchor, rotated by
/// the anchor's road-relative heading. So `du` runs along the road's s axis and
/// `dv` across its t axis, both in metres, and `dyaw` is a rotation about +Z added
/// to the anchor's own heading. On a curved road, offsetting in (s, t) linearly is
/// the same small-offset approximation `distribute_props_along_curve` and the Prop
/// Span tool already make — an assembly spans a few metres, not a few hundred.
///
/// ★ THERE IS NO PITCH OR ROLL, on purpose. `mesh::ObjectInstance` carries a
/// position, a heading about +Z and one uniform scale — so `Object::pitch` and
/// `Object::roll` are parsed, written and round-tripped but ignored by the mesher
/// and both exporters for EVERY prop in the product, not just for assembly parts.
/// A part that needs to lie down (the mast arm) therefore ships as geometry that
/// is already horizontal. Widening the instance is tracked separately rather than
/// smuggled in here.
struct AssemblyPart {
  std::string model;  ///< a `props::model()` id
  double du = 0.0;    ///< offset along the road's s axis [m]
  double dv = 0.0;    ///< offset across the road's t axis [m]
  double dz = 0.0;    ///< offset above the anchor's zOffset [m]
  double dyaw = 0.0;  ///< yaw about +Z, added to the anchor's heading [rad]
  double scale = 1.0; ///< uniform multiplier on this part's model dimensions

  friend bool operator==(const AssemblyPart&, const AssemblyPart&) = default;
};

/// A composite prop asset: a stable id, a label for the Library, and its parts in
/// placement order (which is also the order their `<object>`s are minted, and the
/// order they are recorded in a `SignalMount`).
struct PropAssembly {
  std::string id;
  std::string label;
  std::vector<AssemblyPart> parts;

  friend bool operator==(const PropAssembly&, const PropAssembly&) = default;
};

/// Upper bound on an assembly's part count. Mirrors `kMaxSignalMountParts` for the
/// same reason: it keeps the `rm:assembly` persistence grammar's part index short
/// and lets a reader reject a corrupt record outright instead of allocating on it.
inline constexpr std::size_t kMaxAssemblyParts = 16;

/// Stable ids of every available assembly, in catalogue order: the bundled ones
/// first, then whatever the open project defines.
RM_API const std::vector<std::string>& assembly_ids();

/// The assembly for `id`, or nullptr if unknown. A project's own definitions are
/// consulted BEFORE the bundled ones, so a project may shadow a bundled id.
///
/// ★ POINTER LIFETIME, exactly as for `props::model()`: a bundled assembly's
/// pointer is valid for the program lifetime, a PROJECT assembly's only until the
/// overlay is replaced or cleared. Nothing may cache one across a project switch —
/// and nothing does, because the only consumers (placement and `signalize_junction`)
/// resolve, materialise, and let go within one command.
RM_API const PropAssembly* assembly(std::string_view id);

/// Installs the open project's assembly definitions, REPLACING any previous set.
/// Call on project open and on any change to the project's asset library; pair
/// with `clear_project_assemblies()` on close.
RM_API void register_project_assemblies(std::vector<PropAssembly> assemblies);

/// Drops the project overlay, restoring the bundled catalogue exactly. Every
/// pointer previously returned for a project assembly is invalidated here.
RM_API void clear_project_assemblies();

/// True when `id` resolves to a project assembly rather than a bundled one — so a
/// caller can tell a missing project asset from a missing built-in.
[[nodiscard]] RM_API bool is_project_assembly(std::string_view id);

/// The built-in catalogue, without any project overlay. PRIVATE to the
/// implementation (`src/assets/prop_assembly_registry.cpp` defines both).
namespace detail {
RM_API const std::vector<std::string>& builtin_assembly_ids();
RM_API const PropAssembly* builtin_assembly(std::string_view id);
} // namespace detail

} // namespace roadmaker::props
