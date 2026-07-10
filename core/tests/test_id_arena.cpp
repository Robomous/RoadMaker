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
