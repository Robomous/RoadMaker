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
#include "roadmaker/road/arena.hpp"
#include "roadmaker/road/controller.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/road/signal.hpp"
#include "roadmaker/road/surface.hpp"
#include "roadmaker/road/terrain.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {

/// Owner of all road-network domain objects.
///
/// Objects live in arenas and reference each other exclusively through
/// generational IDs — never pointers. Pointers returned by the lookup
/// methods are invalidated by any mutating call; hold IDs across mutations,
/// re-look-up when needed.
///
/// Single-threaded per network (M1 concurrency contract).
// RM_API is applied per-method, not on the class: a class-level export would
// try to export the Arena<> template members on MSVC. Only the out-of-line
// methods below carry it; the inline lookups need no annotation.
class RoadNetwork {
public:
  // --- creation -----------------------------------------------------------

  RM_API RoadId create_road(std::string name, std::string odr_id);

  RM_API JunctionId create_junction(std::string odr_id, std::string name);

  /// Adds a lane section at s0 [m], keeping Road::sections sorted ascending.
  /// Returns an invalid id if `road` is stale or a section already starts at
  /// exactly this s0.
  RM_API LaneSectionId add_lane_section(RoadId road, double s0);

  /// Adds a lane, keeping LaneSection::lanes sorted by odr lane id
  /// descending (leftmost first). Returns an invalid id if `section` is
  /// stale or the section already has a lane with this odr id.
  RM_API LaneId add_lane(LaneSectionId section, int odr_lane_id, LaneType type);

  /// Adds a road object (OpenDRIVE <object>, §13). `value.road` is
  /// overwritten with `road`. Returns an invalid id if `road` is stale.
  RM_API ObjectId add_object(RoadId road, Object value);

  /// Adds a road signal (OpenDRIVE <signal>, §14). `value.road` is
  /// overwritten with `road`. Returns an invalid id if `road` is stale.
  RM_API SignalId add_signal(RoadId road, Signal value);

  /// Adds a signal controller (OpenDRIVE <controller>, §14.6). Takes NO owner:
  /// a controller is a child of <OpenDRIVE>, not of a road or a junction — a
  /// junction only references it by string id (§12.14).
  RM_API ControllerId add_controller(Controller value);

  /// Creates a ground surface (#215). Unlike add_object, a Surface is not
  /// owned by a single road, so this is a free create. derive_surfaces owns
  /// the surface arena and reconciles it against the enclosed areas.
  RM_API SurfaceId create_surface(Surface value);

  // --- destruction (cascading) --------------------------------------------

  /// Erases the road with its sections, lanes, and objects, and removes
  /// junction connections that reference it. Returns false on a stale id.
  RM_API bool erase_road(RoadId road);

  /// Objects are leaves — nothing references them, so erasing cascades
  /// nothing. Returns false on a stale id.
  RM_API bool erase_object(ObjectId object);

  /// Signals are leaves — nothing references them, so erasing cascades
  /// nothing. Returns false on a stale id.
  RM_API bool erase_signal(SignalId signal);

  /// Controllers are leaves — nothing in the arenas references them (a
  /// <control> and a junction's sync group name signals and controllers by
  /// STRING id), so erasing cascades nothing. Returns false on a stale id.
  /// NOTE the converse too: erase_signal does NOT cascade into controllers. A
  /// <control> naming a gone signal is a dangling reference the validator
  /// reports, exactly as it would be in a third-party file.
  RM_API bool erase_controller(ControllerId controller);

  /// Erases the junction and detaches roads that pointed at it (their
  /// Road::junction becomes invalid). Connecting roads themselves survive.
  /// Returns false on a stale id.
  RM_API bool erase_junction(JunctionId junction);

  /// Surfaces are leaves — nothing references them, so erasing cascades
  /// nothing. Returns false on a stale id. NOTE: erase_road deliberately
  /// does NOT cascade surfaces; they are reconciled by derive_surfaces.
  RM_API bool erase_surface(SurfaceId surface);

  // --- command-layer restore-in-place (roadmaker::edit only) ---------------
  //
  // Exact-slot erase/restore pairs that do NOT bump generations and do NOT
  // cascade or fix up cross-references: an edit command owns the full value
  // snapshot of everything it touches and restores each object itself, so
  // ids held by other domain objects survive undo/redo. Every other caller
  // wants create_*/add_*/erase_* above.

  RM_API Expected<RoadId> restore_road(RoadId id, Road value);
  RM_API Expected<void> erase_road_exact(RoadId id);

  RM_API Expected<LaneSectionId> restore_lane_section(LaneSectionId id, LaneSection value);
  RM_API Expected<void> erase_lane_section_exact(LaneSectionId id);

  RM_API Expected<LaneId> restore_lane(LaneId id, Lane value);
  RM_API Expected<void> erase_lane_exact(LaneId id);

  RM_API Expected<JunctionId> restore_junction(JunctionId id, Junction value);
  RM_API Expected<void> erase_junction_exact(JunctionId id);

  RM_API Expected<ObjectId> restore_object(ObjectId id, Object value);
  RM_API Expected<void> erase_object_exact(ObjectId id);

  RM_API Expected<SignalId> restore_signal(SignalId id, Signal value);
  RM_API Expected<void> erase_signal_exact(SignalId id);

  RM_API Expected<ControllerId> restore_controller(ControllerId id, Controller value);
  RM_API Expected<void> erase_controller_exact(ControllerId id);

  RM_API Expected<SurfaceId> restore_surface(SurfaceId id, Surface value);
  RM_API Expected<void> erase_surface_exact(SurfaceId id);

  // Recycles a slot reserved by an erase_*_exact whose restore never comes,
  // because the command that created the object was DISCARDED rather than
  // reverted-then-reapplied (Command::discard). Command-layer only; guards
  // make a misuse (occupied or already-released slot) a safe no-op error.
  RM_API Expected<void> release_road_reserved(RoadId id);
  RM_API Expected<void> release_lane_section_reserved(LaneSectionId id);
  RM_API Expected<void> release_lane_reserved(LaneId id);
  RM_API Expected<void> release_junction_reserved(JunctionId id);
  RM_API Expected<void> release_object_reserved(ObjectId id);
  RM_API Expected<void> release_signal_reserved(SignalId id);
  RM_API Expected<void> release_controller_reserved(ControllerId id);
  RM_API Expected<void> release_surface_reserved(SurfaceId id);

  // --- lookup (nullptr on stale/invalid ids) ------------------------------

  [[nodiscard]] Road* road(RoadId id) { return roads_.get(id); }

  [[nodiscard]] const Road* road(RoadId id) const { return roads_.get(id); }

  [[nodiscard]] LaneSection* lane_section(LaneSectionId id) { return sections_.get(id); }

  [[nodiscard]] const LaneSection* lane_section(LaneSectionId id) const {
    return sections_.get(id);
  }

  [[nodiscard]] Lane* lane(LaneId id) { return lanes_.get(id); }

  [[nodiscard]] const Lane* lane(LaneId id) const { return lanes_.get(id); }

  [[nodiscard]] Junction* junction(JunctionId id) { return junctions_.get(id); }

  [[nodiscard]] const Junction* junction(JunctionId id) const { return junctions_.get(id); }

  [[nodiscard]] Object* object(ObjectId id) { return objects_.get(id); }

  [[nodiscard]] const Object* object(ObjectId id) const { return objects_.get(id); }

  [[nodiscard]] Signal* signal(SignalId id) { return signals_.get(id); }

  [[nodiscard]] const Signal* signal(SignalId id) const { return signals_.get(id); }

  [[nodiscard]] Controller* controller(ControllerId id) { return controllers_.get(id); }

  [[nodiscard]] const Controller* controller(ControllerId id) const { return controllers_.get(id); }

  [[nodiscard]] Surface* surface(SurfaceId id) { return surfaces_.get(id); }

  [[nodiscard]] const Surface* surface(SurfaceId id) const { return surfaces_.get(id); }

  /// Linear search by OpenDRIVE id; invalid id if absent.
  [[nodiscard]] RM_API RoadId find_road(std::string_view odr_id) const;
  [[nodiscard]] RM_API JunctionId find_junction(std::string_view odr_id) const;

  // --- iteration / stats ----------------------------------------------------

  [[nodiscard]] std::size_t road_count() const { return roads_.size(); }

  [[nodiscard]] std::size_t lane_section_count() const { return sections_.size(); }

  [[nodiscard]] std::size_t lane_count() const { return lanes_.size(); }

  [[nodiscard]] std::size_t junction_count() const { return junctions_.size(); }

  [[nodiscard]] std::size_t object_count() const { return objects_.size(); }

  [[nodiscard]] std::size_t signal_count() const { return signals_.size(); }

  [[nodiscard]] std::size_t controller_count() const { return controllers_.size(); }

  [[nodiscard]] std::size_t surface_count() const { return surfaces_.size(); }

  // Total slots ever allocated per arena (live + erased + reserved). Never
  // shrinks. Observability for reserved-slot leaks: byte-identical xodr cannot
  // see a slot leak (slots are not serialized), so tests watch these instead.
  [[nodiscard]] std::size_t road_slot_count() const { return roads_.slot_count(); }

  [[nodiscard]] std::size_t lane_section_slot_count() const { return sections_.slot_count(); }

  [[nodiscard]] std::size_t lane_slot_count() const { return lanes_.slot_count(); }

  [[nodiscard]] std::size_t junction_slot_count() const { return junctions_.slot_count(); }

  [[nodiscard]] std::size_t object_slot_count() const { return objects_.slot_count(); }

  [[nodiscard]] std::size_t signal_slot_count() const { return signals_.slot_count(); }

  [[nodiscard]] std::size_t controller_slot_count() const { return controllers_.slot_count(); }

  [[nodiscard]] std::size_t surface_slot_count() const { return surfaces_.slot_count(); }

  /// fn(RoadId, Road&) over live roads, in creation order.
  template <class Fn>
  void for_each_road(Fn fn) {
    roads_.for_each(fn);
  }

  template <class Fn>
  void for_each_road(Fn fn) const {
    roads_.for_each(fn);
  }

  template <class Fn>
  void for_each_junction(Fn fn) const {
    junctions_.for_each(fn);
  }

  /// fn(ObjectId, Object&) over live objects, in creation order.
  template <class Fn>
  void for_each_object(Fn fn) {
    objects_.for_each(fn);
  }

  template <class Fn>
  void for_each_object(Fn fn) const {
    objects_.for_each(fn);
  }

  /// fn(SignalId, Signal&) over live signals, in creation order.
  template <class Fn>
  void for_each_signal(Fn fn) {
    signals_.for_each(fn);
  }

  template <class Fn>
  void for_each_signal(Fn fn) const {
    signals_.for_each(fn);
  }

  /// fn(ControllerId, Controller&) over live controllers, in creation order.
  template <class Fn>
  void for_each_controller(Fn fn) {
    controllers_.for_each(fn);
  }

  template <class Fn>
  void for_each_controller(Fn fn) const {
    controllers_.for_each(fn);
  }

  /// fn(SurfaceId, Surface&) over live surfaces, in creation order.
  template <class Fn>
  void for_each_surface(Fn fn) {
    surfaces_.for_each(fn);
  }

  template <class Fn>
  void for_each_surface(Fn fn) const {
    surfaces_.for_each(fn);
  }

  // --- scene height field (p5-s2, #232) -------------------------------------

  /// The ONE scene height field. NOT an arena entity — there is exactly one
  /// per network, and it is scene data rather than a road-network object.
  /// Default-constructed = EMPTY = "this scene has no terrain", which every
  /// consumer reproduces as today's flat-plate behaviour, bit for bit.
  [[nodiscard]] const HeightField& terrain() const { return terrain_; }

  /// Replaces the height field wholesale. Callers outside tests go through the
  /// edit layer (edit::set_terrain_field and friends) so the change is undoable.
  RM_API void set_terrain(HeightField field);

private:
  Arena<Road, RoadId> roads_;
  Arena<LaneSection, LaneSectionId> sections_;
  Arena<Lane, LaneId> lanes_;
  Arena<Junction, JunctionId> junctions_;
  Arena<Object, ObjectId> objects_;
  Arena<Signal, SignalId> signals_;
  Arena<Controller, ControllerId> controllers_;
  Arena<Surface, SurfaceId> surfaces_;
  HeightField terrain_;
};

/// Plan-view bounding box of the whole network as {lo_x, lo_y, hi_x, hi_y},
/// enclosing the road SURFACE (each reference line grown by that road's widest
/// half cross-section), not just the reference lines. std::nullopt when no road
/// carries plan-view geometry.
///
/// Used to size a generated height field (make_flat_field) and to bound the
/// terrain mesh; kept here rather than in the mesher because it is a property
/// of the network, and the mesher is not the only consumer.
[[nodiscard]] RM_API std::optional<std::array<double, 4>>
network_plan_bounds(const RoadNetwork& network);

/// Junctions the road participates in: as a connecting road
/// (Road::junction), through any junction connection (incoming or
/// connecting side), or via its predecessor/successor links. Deduplicated,
/// stable order. Linear scan — junction counts stay small at M2 scale; the
/// edit layer uses this to fill DirtySet::junctions.
[[nodiscard]] RM_API std::vector<JunctionId> junctions_touching(const RoadNetwork& network,
                                                                RoadId road);

/// Objects the road owns, in arena order. Linear scan — object counts stay
/// small at GS-1 scale (docs/design/m3a/01 §2.1).
[[nodiscard]] RM_API std::vector<ObjectId> objects_of(const RoadNetwork& network, RoadId road);

/// Signals the road owns, in arena order. Linear scan — signal counts stay
/// small at GS-1 scale (docs/design/m3a/01 §2.1).
[[nodiscard]] RM_API std::vector<SignalId> signals_of(const RoadNetwork& network, RoadId road);

/// Surfaces whose `bounding_roads` ring contains `road`, in arena order. The
/// regen layer uses this to find the surfaces a road change may disturb.
/// Linear scan — surface counts stay small at P2 scale.
[[nodiscard]] RM_API std::vector<SurfaceId> surfaces_touching(const RoadNetwork& network,
                                                              RoadId road);

/// The lane section governing global station `s` on `road`: the last section
/// whose s0 is <= s. A section is valid from its s0 until the next one
/// begins (OpenDRIVE 1.9.0 §11.4 / 1.8.1 §11.4), so a station before the
/// first section still resolves to it rather than to nothing.
///
/// Returns an invalid id if `road` is stale or has no sections. Linear scan:
/// section counts per road stay small, and `Road::sections` is sorted.
[[nodiscard]] RM_API LaneSectionId section_at(const RoadNetwork& network, RoadId road, double s);

/// End station of `section` along its road: the next section's s0, or the
/// road length for the last section.
///
/// `LaneSection` deliberately stores only s0 — the end is implied. Deriving
/// it is a three-line loop that was open-coded at every call site, so it
/// lives here once. Fails on a stale id or a broken road back-reference.
[[nodiscard]] RM_API Expected<double> section_end(const RoadNetwork& network,
                                                  LaneSectionId section);

/// Lateral t of every lane boundary of `section` at global station `s`,
/// leftmost first: the center boundary sits at laneOffset(s), each left lane
/// adds its width outward (t increases), each right lane subtracts it. Widths
/// are clamped at 0, so a lane already tapered to zero contributes a coincident
/// boundary. Boundary count = lanes + 1.
///
/// This is the one routine that turns a cross section into lane edges; the
/// mesher builds its vertex grid from it and the editor's boundary pick reads
/// it, so lane edges can never drift between what is drawn and what is picked.
[[nodiscard]] RM_API std::vector<double> lane_boundary_offsets(const RoadNetwork& network,
                                                               const Road& road,
                                                               const LaneSection& section,
                                                               double s);

/// Convenience overload resolving the section governing global station `s`
/// (section_at). Returns an empty vector if `road` is stale or has no sections.
[[nodiscard]] RM_API std::vector<double>
lane_boundary_offsets(const RoadNetwork& network, RoadId road, double s);

} // namespace roadmaker
