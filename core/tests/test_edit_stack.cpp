#include "roadmaker/edit/edit_stack.hpp"
#include "roadmaker/road/network.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

using roadmaker::ErrorCode;
using roadmaker::Expected;
using roadmaker::RoadId;
using roadmaker::RoadNetwork;
using roadmaker::edit::Command;
using roadmaker::edit::DirtySet;
using roadmaker::edit::EditStack;

namespace {

/// Renames a road; the network mutation makes apply/revert observable.
class RenameCommand final : public Command {
public:
  RenameCommand(RoadId road, std::string from, std::string to)
      : road_(road), from_(std::move(from)), to_(std::move(to)) {}

  Expected<void> apply(RoadNetwork& network) override {
    roadmaker::Road* road = network.road(road_);
    if (road == nullptr) {
      return roadmaker::make_error(ErrorCode::InvalidArgument, "stale road");
    }
    road->name = to_;
    return {};
  }

  Expected<void> revert(RoadNetwork& network) override {
    network.road(road_)->name = from_;
    return {};
  }

  std::string_view name() const override { return "Rename Road"; }

  DirtySet dirty() const override { return DirtySet{.roads = {road_}}; }

private:
  RoadId road_;
  std::string from_;
  std::string to_;
};

/// Always fails apply without touching the network.
class FailingCommand final : public Command {
public:
  Expected<void> apply(RoadNetwork&) override {
    return roadmaker::make_error(ErrorCode::InvalidArgument, "doomed");
  }

  Expected<void> revert(RoadNetwork&) override { return {}; }

  std::string_view name() const override { return "Failing"; }

  DirtySet dirty() const override { return {}; }
};

std::unique_ptr<Command> rename(RoadId road, std::string from, std::string to) {
  return std::make_unique<RenameCommand>(road, std::move(from), std::move(to));
}

} // namespace

TEST(EditStack, PushAppliesAndRecords) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;

  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());
  EXPECT_EQ(network.road(road)->name, "b");
  EXPECT_TRUE(stack.can_undo());
  EXPECT_FALSE(stack.can_redo());
  EXPECT_EQ(stack.size(), 1U);
}

TEST(EditStack, UndoRevertsAndRedoReapplies) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;
  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());

  ASSERT_TRUE(stack.undo(network).has_value());
  EXPECT_EQ(network.road(road)->name, "a");
  EXPECT_FALSE(stack.can_undo());
  EXPECT_TRUE(stack.can_redo());

  ASSERT_TRUE(stack.redo(network).has_value());
  EXPECT_EQ(network.road(road)->name, "b");
  EXPECT_TRUE(stack.can_undo());
  EXPECT_FALSE(stack.can_redo());
}

TEST(EditStack, PushTruncatesTheRedoTail) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;
  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());
  ASSERT_TRUE(stack.push(network, rename(road, "b", "c")).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());
  ASSERT_TRUE(stack.can_redo());

  ASSERT_TRUE(stack.push(network, rename(road, "b", "z")).has_value());
  EXPECT_FALSE(stack.can_redo());
  EXPECT_EQ(stack.size(), 2U);
  EXPECT_EQ(network.road(road)->name, "z");
}

TEST(EditStack, FailedApplyIsNotRecorded) {
  RoadNetwork network;
  network.create_road("a", "1");
  EditStack stack;

  const auto result = stack.push(network, std::make_unique<FailingCommand>());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
  EXPECT_FALSE(stack.can_undo());
  EXPECT_EQ(stack.size(), 0U);
}

TEST(EditStack, RejectsNullCommandAndEmptyUndoRedo) {
  RoadNetwork network;
  EditStack stack;
  EXPECT_FALSE(stack.push(network, nullptr).has_value());
  EXPECT_FALSE(stack.undo(network).has_value());
  EXPECT_FALSE(stack.redo(network).has_value());
}

TEST(EditStack, DepthLimitDropsOldestButKeepsTheirEditsApplied) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;
  stack.set_depth_limit(2);

  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());
  ASSERT_TRUE(stack.push(network, rename(road, "b", "c")).has_value());
  ASSERT_TRUE(stack.push(network, rename(road, "c", "d")).has_value());
  EXPECT_EQ(stack.size(), 2U);

  // Only the two newest commands can be undone; the oldest edit sticks.
  ASSERT_TRUE(stack.undo(network).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());
  EXPECT_FALSE(stack.can_undo());
  EXPECT_EQ(network.road(road)->name, "b");
}

TEST(EditStack, SetDepthLimitTrimsExistingHistoryAndClampsToOne) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;
  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());
  ASSERT_TRUE(stack.push(network, rename(road, "b", "c")).has_value());

  stack.set_depth_limit(0); // clamped
  EXPECT_EQ(stack.depth_limit(), 1U);
  EXPECT_EQ(stack.size(), 1U);
  ASSERT_TRUE(stack.undo(network).has_value());
  EXPECT_FALSE(stack.can_undo());
  EXPECT_EQ(network.road(road)->name, "b");
}

TEST(EditStack, ClearForgetsEverything) {
  RoadNetwork network;
  const RoadId road = network.create_road("a", "1");
  EditStack stack;
  ASSERT_TRUE(stack.push(network, rename(road, "a", "b")).has_value());
  ASSERT_TRUE(stack.undo(network).has_value());

  stack.clear();
  EXPECT_FALSE(stack.can_undo());
  EXPECT_FALSE(stack.can_redo());
  EXPECT_EQ(stack.size(), 0U);
}
