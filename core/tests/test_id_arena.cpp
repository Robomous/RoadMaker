// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/road/arena.hpp"
#include "roadmaker/road/id.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace {

struct WidgetTag;
using WidgetId = roadmaker::Id<WidgetTag>;

struct Widget {
  std::string name;
};

using WidgetArena = roadmaker::Arena<Widget, WidgetId>;

} // namespace

TEST(Id, DefaultConstructedIsInvalid) {
  constexpr WidgetId kDefault;
  static_assert(!kDefault.is_valid());
}

TEST(Id, ComparesByIndexAndGeneration) {
  constexpr WidgetId kA{.index = 1, .gen = 0};
  constexpr WidgetId kB{.index = 1, .gen = 1};
  constexpr WidgetId kC{.index = 1, .gen = 0};
  static_assert(kA != kB);
  static_assert(kA == kC);
}

TEST(Id, HashableAndDistinct) {
  std::unordered_set<WidgetId> set;
  set.insert(WidgetId{.index = 0, .gen = 0});
  set.insert(WidgetId{.index = 0, .gen = 1});
  set.insert(WidgetId{.index = 1, .gen = 0});
  EXPECT_EQ(set.size(), 3U);
}

TEST(Arena, EmplaceAndGetRoundTrip) {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "a"});
  EXPECT_TRUE(id.is_valid());
  EXPECT_EQ(arena.size(), 1U);
  ASSERT_NE(arena.get(id), nullptr);
  EXPECT_EQ(arena.get(id)->name, "a");
}

TEST(Arena, GetOnInvalidOrForeignIdsReturnsNull) {
  WidgetArena arena;
  EXPECT_EQ(arena.get(WidgetId{}), nullptr);
  EXPECT_EQ(arena.get(WidgetId{.index = 42, .gen = 0}), nullptr);
}

TEST(Arena, EraseInvalidatesHandleAndFreesSlot) {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "doomed"});
  EXPECT_TRUE(arena.erase(id));
  EXPECT_EQ(arena.get(id), nullptr);
  EXPECT_TRUE(arena.empty());
  EXPECT_FALSE(arena.erase(id)); // double erase is a no-op
}

TEST(Arena, SlotReuseBumpsGeneration) {
  WidgetArena arena;
  const WidgetId old_id = arena.emplace(Widget{.name = "old"});
  arena.erase(old_id);

  const WidgetId new_id = arena.emplace(Widget{.name = "new"});
  EXPECT_EQ(new_id.index, old_id.index); // slot reused
  EXPECT_NE(new_id.gen, old_id.gen);

  EXPECT_EQ(arena.get(old_id), nullptr); // stale handle stays dead
  ASSERT_NE(arena.get(new_id), nullptr);
  EXPECT_EQ(arena.get(new_id)->name, "new");
}

TEST(Arena, EraseExactThenRestoreResurrectsUnderOriginalId) {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "phoenix"});

  ASSERT_TRUE(arena.erase_exact(id).has_value());
  EXPECT_EQ(arena.get(id), nullptr);
  EXPECT_TRUE(arena.empty());

  const auto restored = arena.restore(id, Widget{.name = "phoenix"});
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, id); // same index AND same generation
  ASSERT_NE(arena.get(id), nullptr);
  EXPECT_EQ(arena.get(id)->name, "phoenix");
  EXPECT_EQ(arena.size(), 1U);
}

TEST(Arena, EraseExactSlotIsNeverRecycledByEmplace) {
  WidgetArena arena;
  const WidgetId reserved = arena.emplace(Widget{.name = "reserved"});
  ASSERT_TRUE(arena.erase_exact(reserved).has_value());

  // A new emplace must not alias the resurrectable slot.
  const WidgetId fresh = arena.emplace(Widget{.name = "fresh"});
  EXPECT_NE(fresh.index, reserved.index);
  ASSERT_TRUE(arena.restore(reserved, Widget{.name = "reserved"}).has_value());
  EXPECT_EQ(arena.size(), 2U);
}

TEST(Arena, ReleaseReservedRecyclesSlotWithFreshGeneration) {
  WidgetArena arena;
  const WidgetId reserved = arena.emplace(Widget{.name = "reserved"});
  ASSERT_TRUE(arena.erase_exact(reserved).has_value());
  EXPECT_EQ(arena.size(), 0U);
  EXPECT_EQ(arena.slot_count(), 1U); // slot still reserved, not recycled

  ASSERT_TRUE(arena.release_reserved(reserved).has_value());
  EXPECT_EQ(arena.size(), 0U);       // alive_ untouched (erase_exact decremented)
  EXPECT_EQ(arena.slot_count(), 1U); // storage never shrinks

  // The released slot is now reusable — next emplace takes its index with a
  // fresh generation, and no new storage grows.
  const WidgetId fresh = arena.emplace(Widget{.name = "fresh"});
  EXPECT_EQ(fresh.index, reserved.index);
  EXPECT_NE(fresh.gen, reserved.gen);
  EXPECT_EQ(arena.size(), 1U);
  EXPECT_EQ(arena.slot_count(), 1U);
}

TEST(Arena, ReleaseReservedRejectsOccupiedUnknownAndGenMismatchedSlots) {
  WidgetArena arena;
  const WidgetId occupied = arena.emplace(Widget{.name = "here"});
  EXPECT_FALSE(arena.release_reserved(occupied).has_value()); // slot is live

  EXPECT_FALSE(arena.release_reserved(WidgetId{.index = 42, .gen = 0}).has_value()); // out of range

  // A slot freed by plain erase already recycled its index and bumped the
  // generation, so the old handle fails the generation check.
  const WidgetId erased = arena.emplace(Widget{.name = "gone"});
  arena.erase(erased);
  EXPECT_FALSE(arena.release_reserved(erased).has_value());

  // Double release: the first bumped the generation, so the second no-ops.
  const WidgetId twice = arena.emplace(Widget{.name = "twice"});
  ASSERT_TRUE(arena.erase_exact(twice).has_value());
  ASSERT_TRUE(arena.release_reserved(twice).has_value());
  EXPECT_FALSE(arena.release_reserved(twice).has_value());
}

TEST(Arena, RestoreAfterReleaseFails) {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "w"});
  ASSERT_TRUE(arena.erase_exact(id).has_value());
  ASSERT_TRUE(arena.release_reserved(id).has_value());

  // release_reserved bumped the generation, so the paired restore now fails.
  EXPECT_FALSE(arena.restore(id, Widget{.name = "w"}).has_value());

  // The slot is still a normal free slot — emplace reuses it.
  const WidgetId reused = arena.emplace(Widget{.name = "next"});
  EXPECT_EQ(reused.index, id.index);
  EXPECT_NE(reused.gen, id.gen);
}

TEST(Arena, EraseExactRejectsInvalidAndStaleIds) {
  WidgetArena arena;
  EXPECT_FALSE(arena.erase_exact(WidgetId{}).has_value());

  const WidgetId id = arena.emplace(Widget{.name = "once"});
  arena.erase(id); // plain erase bumps the generation
  EXPECT_FALSE(arena.erase_exact(id).has_value());
}

TEST(Arena, RestoreRejectsOccupiedUnknownAndGenMismatchedSlots) {
  WidgetArena arena;
  const WidgetId occupied = arena.emplace(Widget{.name = "here"});
  EXPECT_FALSE(arena.restore(occupied, Widget{.name = "clone"}).has_value());

  EXPECT_FALSE(arena.restore(WidgetId{.index = 42, .gen = 0}, Widget{}).has_value());

  // A slot freed by plain erase fails the generation check: its slot gen was
  // bumped, so the old handle can never be restored into it.
  const WidgetId erased = arena.emplace(Widget{.name = "gone"});
  arena.erase(erased);
  EXPECT_FALSE(arena.restore(erased, Widget{.name = "gone"}).has_value());
}

TEST(Arena, RestoredSlotBehavesNormallyAfterwards) {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "w"});
  ASSERT_TRUE(arena.erase_exact(id).has_value());
  ASSERT_TRUE(arena.restore(id, Widget{.name = "w"}).has_value());

  // Plain erase on the restored slot bumps the generation as usual.
  EXPECT_TRUE(arena.erase(id));
  const WidgetId reused = arena.emplace(Widget{.name = "next"});
  EXPECT_EQ(reused.index, id.index);
  EXPECT_NE(reused.gen, id.gen);
}

TEST(Arena, ForEachVisitsLiveElementsInSlotOrder) {
  WidgetArena arena;
  const WidgetId a = arena.emplace(Widget{.name = "a"});
  const WidgetId b = arena.emplace(Widget{.name = "b"});
  arena.emplace(Widget{.name = "c"});
  arena.erase(b);

  std::vector<std::string> visited;
  arena.for_each([&](WidgetId id, const Widget& widget) {
    EXPECT_TRUE(id.is_valid());
    visited.push_back(widget.name);
  });
  EXPECT_EQ(visited, (std::vector<std::string>{"a", "c"}));
  ASSERT_NE(arena.get(a), nullptr);
  EXPECT_EQ(arena.get(a)->name, "a");
}
