#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace roadmaker {

/// Generational arena handle. A default-constructed Id is invalid; an Id
/// becomes stale (lookups return nullptr) once its slot is erased.
template <class Tag>
struct Id {
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFU;

  std::uint32_t index = kInvalidIndex;
  std::uint32_t gen = 0;

  [[nodiscard]] constexpr bool is_valid() const { return index != kInvalidIndex; }

  friend constexpr bool operator==(Id, Id) = default;
};

struct RoadTag;
struct LaneSectionTag;
struct LaneTag;
struct JunctionTag;

using RoadId = Id<RoadTag>;
using LaneSectionId = Id<LaneSectionTag>;
using LaneId = Id<LaneTag>;
using JunctionId = Id<JunctionTag>;

} // namespace roadmaker

template <class Tag>
struct std::hash<roadmaker::Id<Tag>> {
  [[nodiscard]] std::size_t operator()(roadmaker::Id<Tag> id) const noexcept {
    return (static_cast<std::size_t>(id.gen) << 32U) | id.index;
  }
};
