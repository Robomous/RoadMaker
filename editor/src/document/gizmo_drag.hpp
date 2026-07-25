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

// The transform gizmo's DRAG, separated from its rendering (p6-s15, #417).
//
// Everything here is "which entity, which handle, what edit" — no camera, no
// painter, no GL — so the gesture is unit-testable headless, which is what the
// sprint's acceptance asks for and what the project's widgets-stay-thin rule
// has always asked for. ViewportWidget keeps only what genuinely needs a
// camera: hit-testing the handles on screen, un-projecting the cursor to the
// ground plane, the AxisZ pixels-per-metre scale, painting, and mapping the
// suppression modifier to `free_rotation`.
//
// The lifecycle is Document's preview session (docs/design/m2/01_editing_framework.md
// §3): live preview per frame, exactly ONE undo entry on commit, byte-identical
// revert on cancel. No mergeWith.

#include "roadmaker/error.hpp"
#include "roadmaker/road/id.hpp"

#include <QString>
#include <array>
#include <optional>

#include "document/selection_model.hpp"
#include "viewport/gizmo.hpp" // GizmoHandle

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

class Document;

/// The single transformable entity under the gizmo — exactly one of road,
/// object or signal — with its world pivot.
struct GizmoTarget {
  RoadId road;
  ObjectId object;
  SignalId signal;
  std::array<double, 3> pivot{};
};

/// The gizmo target for `entry`, or nullopt when the entry transforms nothing.
///
/// ORDER IS LOAD-BEARING: a leaf entity wins over the road it carries as a
/// back-reference. A prop pick and a signal pick both set `.road` to the owning
/// road (picking.hpp), so testing `.road` first would resolve a selected SIGN
/// to its road — which is exactly the bug this sprint fixes: before #417 a
/// selected sign drew the road's gizmo at the road midpoint, and its ring
/// rotated the whole road. SelectionModel::selected_roads() has carried the
/// same guard all along.
///
/// Junctions and surfaces are unreachable here by construction: their picks
/// leave `.road` invalid, so they simply fall through to nullopt.
[[nodiscard]] std::optional<GizmoTarget> gizmo_target(const RoadNetwork& network,
                                                      const SelectionEntry& entry);

/// One frame of drag input, already resolved out of screen space by the caller.
struct GizmoDragInput {
  /// Cursor on the z=0 ground plane [m].
  std::array<double, 2> cursor_world{};
  /// AxisZ only: the world-Z delta from the press [m], which only the widget
  /// can compute (it needs the pivot's screen pixels-per-metre).
  double dz = 0.0;
  /// The suppression modifier is held — rotate freely instead of snapping.
  bool free_rotation = false;
};

/// A gizmo drag, from press to release, over a Document's preview session.
///
/// ROTATION SNAPPING is not uniform, and deliberately so (spec's
/// auto-orientation section; the increment is defaults::kPropRotationSnap):
///   - props and signals snap the RESULTING road-relative angle, so a prop
///     lands on an exact multiple of the increment relative to the road (a
///     bench ends up exactly perpendicular, not perpendicular-plus-whatever-
///     it-started-with);
///   - roads snap the DELTA they are rotated by, because edit::rotate_road
///     turns a road about a pivot and a road has no single heading to be
///     absolute about.
///
/// A signal's ring writes @hOffset ONLY; @orientation is never touched. Under
/// OpenDRIVE §14.1 @orientation declares which traffic the signal APPLIES to,
/// so a visual gesture must not silently change what a sign governs. That also
/// keeps the ring outside the three sites allowed to DERIVE a facing
/// (roadmaker/road/signal_facing.hpp) — a heading dragged here is an override
/// and nothing recomputes it.
class GizmoDragSession {
public:
  explicit GizmoDragSession(Document& document);

  /// Arms a drag on `handle`. False (and no session) when the handle is None or
  /// an axis the target does not offer, leaving the press to fall through.
  /// `press_world` is the ground-plane point under the press.
  [[nodiscard]] bool
  begin(const GizmoTarget& target, GizmoHandle handle, std::array<double, 2> press_world);

  /// One drag frame: previews the edit this handle implies. The kernel's
  /// refusal (a junction arm's generated pose, say) is RETURNED rather than
  /// swallowed — surfacing it to the user is #401's, and this is the one place
  /// it has to hook. A frame that yields no command at all (dragged clear of
  /// the road) is not an error: the session simply keeps its last good frame.
  [[nodiscard]] Expected<void> update(const GizmoDragInput& input);

  /// Commits as ONE undo entry, returning whether anything was actually
  /// pushed. A press-and-release that never moved previewed nothing, so it
  /// commits nothing and reports false — the caller's cue not to toast.
  bool commit();

  /// Reverts the live preview (Esc / interruption).
  void cancel();

  [[nodiscard]] bool active() const { return drag_.has_value(); }

  [[nodiscard]] GizmoHandle handle() const;

  /// Toast text for the frame most recently previewed; empty before the first
  /// update().
  [[nodiscard]] QString summary() const;

private:
  /// The press-time state a drag resolves against.
  struct Drag {
    GizmoHandle handle = GizmoHandle::None;
    GizmoTarget target;
    std::array<double, 2> press_world{};
    /// Prop heading / signal heading offset at press [rad] — the base the
    /// absolute snap is applied to.
    double base_angle = 0.0;
    QString summary;
  };

  Document& document_;
  std::optional<Drag> drag_;
};

} // namespace roadmaker::editor
