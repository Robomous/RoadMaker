#include "roadmaker/road/network.hpp"

#include "roadmaker/tol.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace roadmaker {

RoadId RoadNetwork::create_road(std::string name, std::string odr_id) {
  return roads_.emplace(Road{.name = std::move(name), .odr_id = std::move(odr_id)});
}

JunctionId RoadNetwork::create_junction(std::string odr_id, std::string name) {
  return junctions_.emplace(Junction{.odr_id = std::move(odr_id), .name = std::move(name)});
}

LaneSectionId RoadNetwork::add_lane_section(RoadId road_id, double s0) {
  Road* owner = roads_.get(road_id);
  if (owner == nullptr) {
    return {};
  }
  const auto pos = std::ranges::lower_bound(
      owner->sections, s0, {}, [this](LaneSectionId id) { return sections_.get(id)->s0; });
  if (pos != owner->sections.end() && sections_.get(*pos)->s0 == s0) {
    return {}; // a section already starts exactly here
  }
  const LaneSectionId id = sections_.emplace(LaneSection{.road = road_id, .s0 = s0});
  // emplace may not invalidate `pos` (different arena), but re-fetch the
  // road pointer discipline applies to arenas of the same type only; the
  // sections_ mutation cannot move roads_ storage.
  owner->sections.insert(pos, id);
  return id;
}

LaneId RoadNetwork::add_lane(LaneSectionId section_id, int odr_lane_id, LaneType type) {
  LaneSection* owner = sections_.get(section_id);
  if (owner == nullptr) {
    return {};
  }
  // Descending order: leftmost (largest odr id) first.
  const auto pos = std::ranges::lower_bound(owner->lanes,
                                            odr_lane_id,
                                            std::ranges::greater{},
                                            [this](LaneId id) { return lanes_.get(id)->odr_id; });
  if (pos != owner->lanes.end() && lanes_.get(*pos)->odr_id == odr_lane_id) {
    return {}; // duplicate lane id within the section
  }
  const LaneId id =
      lanes_.emplace(Lane{.section = section_id, .odr_id = odr_lane_id, .type = type});
  owner->lanes.insert(pos, id);
  return id;
}

ObjectId RoadNetwork::add_object(RoadId road_id, Object value) {
  if (roads_.get(road_id) == nullptr) {
    return {};
  }
  value.road = road_id;
  return objects_.emplace(std::move(value));
}

SignalId RoadNetwork::add_signal(RoadId road_id, Signal value) {
  if (roads_.get(road_id) == nullptr) {
    return {};
  }
  value.road = road_id;
  return signals_.emplace(std::move(value));
}

bool RoadNetwork::erase_road(RoadId road_id) {
  Road* doomed = roads_.get(road_id);
  if (doomed == nullptr) {
    return false;
  }
  for (const LaneSectionId section_id : doomed->sections) {
    if (const LaneSection* section = sections_.get(section_id)) {
      for (const LaneId lane_id : section->lanes) {
        lanes_.erase(lane_id);
      }
      sections_.erase(section_id);
    }
  }
  // Objects are owned by their road: collect first, then erase — for_each
  // must not mutate the arena it iterates.
  std::vector<ObjectId> owned;
  objects_.for_each([&](ObjectId id, const Object& object) {
    if (object.road == road_id) {
      owned.push_back(id);
    }
  });
  for (const ObjectId id : owned) {
    objects_.erase(id);
  }
  // Signals are owned by their road too — same collect-then-erase discipline.
  std::vector<SignalId> owned_signals;
  signals_.for_each([&](SignalId id, const Signal& signal) {
    if (signal.road == road_id) {
      owned_signals.push_back(id);
    }
  });
  for (const SignalId id : owned_signals) {
    signals_.erase(id);
  }
  // Keep junctions coherent: drop connections that reference this road.
  junctions_.for_each([road_id](JunctionId, Junction& junction) {
    std::erase_if(junction.connections, [road_id](const JunctionConnection& connection) {
      return connection.incoming_road == road_id || connection.connecting_road == road_id;
    });
  });
  return roads_.erase(road_id);
}

bool RoadNetwork::erase_object(ObjectId object_id) {
  return objects_.erase(object_id);
}

bool RoadNetwork::erase_signal(SignalId signal_id) {
  return signals_.erase(signal_id);
}

bool RoadNetwork::erase_junction(JunctionId junction_id) {
  if (junctions_.get(junction_id) == nullptr) {
    return false;
  }
  roads_.for_each([junction_id](RoadId, Road& road) {
    if (road.junction == junction_id) {
      road.junction = {};
    }
  });
  return junctions_.erase(junction_id);
}

Expected<RoadId> RoadNetwork::restore_road(RoadId id, Road value) {
  return roads_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_road_exact(RoadId id) {
  return roads_.erase_exact(id);
}

Expected<LaneSectionId> RoadNetwork::restore_lane_section(LaneSectionId id, LaneSection value) {
  return sections_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_lane_section_exact(LaneSectionId id) {
  return sections_.erase_exact(id);
}

Expected<LaneId> RoadNetwork::restore_lane(LaneId id, Lane value) {
  return lanes_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_lane_exact(LaneId id) {
  return lanes_.erase_exact(id);
}

Expected<JunctionId> RoadNetwork::restore_junction(JunctionId id, Junction value) {
  return junctions_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_junction_exact(JunctionId id) {
  return junctions_.erase_exact(id);
}

Expected<ObjectId> RoadNetwork::restore_object(ObjectId id, Object value) {
  return objects_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_object_exact(ObjectId id) {
  return objects_.erase_exact(id);
}

Expected<SignalId> RoadNetwork::restore_signal(SignalId id, Signal value) {
  return signals_.restore(id, std::move(value));
}

Expected<void> RoadNetwork::erase_signal_exact(SignalId id) {
  return signals_.erase_exact(id);
}

RoadId RoadNetwork::find_road(std::string_view odr_id) const {
  RoadId found;
  roads_.for_each([&](RoadId id, const Road& road) {
    if (!found.is_valid() && road.odr_id == odr_id) {
      found = id;
    }
  });
  return found;
}

JunctionId RoadNetwork::find_junction(std::string_view odr_id) const {
  JunctionId found;
  junctions_.for_each([&](JunctionId id, const Junction& junction) {
    if (!found.is_valid() && junction.odr_id == odr_id) {
      found = id;
    }
  });
  return found;
}

std::vector<JunctionId> junctions_touching(const RoadNetwork& network, RoadId road_id) {
  std::vector<JunctionId> touched;
  const auto note = [&touched](JunctionId id) {
    if (id.is_valid() && std::ranges::find(touched, id) == touched.end()) {
      touched.push_back(id);
    }
  };

  if (const Road* road = network.road(road_id)) {
    note(road->junction);
    const auto note_link = [&note](const std::optional<RoadLink>& link) {
      if (link.has_value()) {
        if (const auto* junction = std::get_if<JunctionId>(&link->target)) {
          note(*junction);
        }
      }
    };
    note_link(road->predecessor);
    note_link(road->successor);
  }

  network.for_each_junction([&](JunctionId id, const Junction& junction) {
    for (const JunctionConnection& connection : junction.connections) {
      if (connection.incoming_road == road_id || connection.connecting_road == road_id) {
        note(id);
        break;
      }
    }
  });
  return touched;
}

std::vector<ObjectId> objects_of(const RoadNetwork& network, RoadId road_id) {
  std::vector<ObjectId> owned;
  network.for_each_object([&](ObjectId id, const Object& object) {
    if (object.road == road_id) {
      owned.push_back(id);
    }
  });
  return owned;
}

std::vector<SignalId> signals_of(const RoadNetwork& network, RoadId road_id) {
  std::vector<SignalId> owned;
  network.for_each_signal([&](SignalId id, const Signal& signal) {
    if (signal.road == road_id) {
      owned.push_back(id);
    }
  });
  return owned;
}

LaneSectionId section_at(const RoadNetwork& network, RoadId road_id, double s) {
  const Road* road = network.road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return LaneSectionId{};
  }
  // Sections are sorted ascending by s0, so the last one starting at or
  // before s governs it. Seeding with the first section makes a query before
  // the road start resolve to it instead of returning nothing.
  LaneSectionId result = road->sections.front();
  for (const LaneSectionId id : road->sections) {
    const LaneSection* section = network.lane_section(id);
    if (section == nullptr) {
      continue;
    }
    if (section->s0 <= s + tol::kLength) {
      result = id;
    }
  }
  return result;
}

Expected<double> section_end(const RoadNetwork& network, LaneSectionId section_id) {
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale lane-section id");
  }
  const Road* road = network.road(section->road);
  if (road == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "lane section has a stale road back-reference");
  }
  const auto here = std::ranges::find(road->sections, section_id);
  if (here == road->sections.end()) {
    return make_error(ErrorCode::InvalidArgument, "lane section is not listed on its road");
  }
  const auto next = std::next(here);
  if (next == road->sections.end()) {
    return road->length;
  }
  const LaneSection* following = network.lane_section(*next);
  if (following == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "the following lane section is stale");
  }
  return following->s0;
}

} // namespace roadmaker
