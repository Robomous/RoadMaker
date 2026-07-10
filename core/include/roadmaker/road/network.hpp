#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/arena.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/road.hpp"

#include <string>
#include <string_view>

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

  // --- destruction (cascading) --------------------------------------------

  /// Erases the road with its sections and lanes, and removes junction
  /// connections that reference it. Returns false on a stale id.
  RM_API bool erase_road(RoadId road);

  /// Erases the junction and detaches roads that pointed at it (their
  /// Road::junction becomes invalid). Connecting roads themselves survive.
  /// Returns false on a stale id.
  RM_API bool erase_junction(JunctionId junction);

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

  /// Linear search by OpenDRIVE id; invalid id if absent.
  [[nodiscard]] RM_API RoadId find_road(std::string_view odr_id) const;
  [[nodiscard]] RM_API JunctionId find_junction(std::string_view odr_id) const;

  // --- iteration / stats ----------------------------------------------------

  [[nodiscard]] std::size_t road_count() const { return roads_.size(); }

  [[nodiscard]] std::size_t lane_section_count() const { return sections_.size(); }

  [[nodiscard]] std::size_t lane_count() const { return lanes_.size(); }

  [[nodiscard]] std::size_t junction_count() const { return junctions_.size(); }

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

private:
  Arena<Road, RoadId> roads_;
  Arena<LaneSection, LaneSectionId> sections_;
  Arena<Lane, LaneId> lanes_;
  Arena<Junction, JunctionId> junctions_;
};

} // namespace roadmaker
