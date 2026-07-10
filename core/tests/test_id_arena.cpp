#include "roadmaker/road/arena.hpp"
#include "roadmaker/road/id.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("default-constructed Id is invalid", "[id]") {
  constexpr WidgetId id;
  STATIC_REQUIRE(!id.is_valid());
}

TEST_CASE("Ids compare by index and generation", "[id]") {
  constexpr WidgetId a{.index = 1, .gen = 0};
  constexpr WidgetId b{.index = 1, .gen = 1};
  constexpr WidgetId c{.index = 1, .gen = 0};
  STATIC_REQUIRE(a != b);
  STATIC_REQUIRE(a == c);
}

TEST_CASE("Ids are hashable and distinct", "[id]") {
  std::unordered_set<WidgetId> set;
  set.insert(WidgetId{.index = 0, .gen = 0});
  set.insert(WidgetId{.index = 0, .gen = 1});
  set.insert(WidgetId{.index = 1, .gen = 0});
  REQUIRE(set.size() == 3);
}

TEST_CASE("arena emplace and get round-trip", "[arena]") {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "a"});
  REQUIRE(id.is_valid());
  REQUIRE(arena.size() == 1);
  REQUIRE(arena.get(id) != nullptr);
  REQUIRE(arena.get(id)->name == "a");
}

TEST_CASE("get on invalid or foreign ids returns nullptr", "[arena]") {
  WidgetArena arena;
  REQUIRE(arena.get(WidgetId{}) == nullptr);
  REQUIRE(arena.get(WidgetId{.index = 42, .gen = 0}) == nullptr);
}

TEST_CASE("erase invalidates the handle and frees the slot", "[arena]") {
  WidgetArena arena;
  const WidgetId id = arena.emplace(Widget{.name = "doomed"});
  REQUIRE(arena.erase(id));
  REQUIRE(arena.get(id) == nullptr);
  REQUIRE(arena.empty());
  REQUIRE_FALSE(arena.erase(id)); // double erase is a no-op
}

TEST_CASE("slot reuse bumps the generation", "[arena]") {
  WidgetArena arena;
  const WidgetId old_id = arena.emplace(Widget{.name = "old"});
  arena.erase(old_id);

  const WidgetId new_id = arena.emplace(Widget{.name = "new"});
  REQUIRE(new_id.index == old_id.index); // slot reused
  REQUIRE(new_id.gen != old_id.gen);

  REQUIRE(arena.get(old_id) == nullptr); // stale handle stays dead
  REQUIRE(arena.get(new_id)->name == "new");
}

TEST_CASE("for_each visits live elements in slot order", "[arena]") {
  WidgetArena arena;
  const WidgetId a = arena.emplace(Widget{.name = "a"});
  const WidgetId b = arena.emplace(Widget{.name = "b"});
  arena.emplace(Widget{.name = "c"});
  arena.erase(b);

  std::vector<std::string> visited;
  arena.for_each([&](WidgetId id, const Widget& widget) {
    REQUIRE(id.is_valid());
    visited.push_back(widget.name);
  });
  REQUIRE(visited == std::vector<std::string>{"a", "c"});
  REQUIRE(arena.get(a)->name == "a");
}
