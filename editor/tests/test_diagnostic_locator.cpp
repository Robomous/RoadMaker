#include <gtest/gtest.h>

#include "document/diagnostic_locator.hpp"

namespace roadmaker::editor {
namespace {

/// Two roads in document order; road 1 has one section with lanes 1 and -1.
class DiagnosticLocatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    road_a_ = network_.create_road("first", "10");
    road_b_ = network_.create_road("second", "20");
    section_ = network_.add_lane_section(road_b_, 0.0);
    lane_left_ = network_.add_lane(section_, 1, LaneType::Driving);
    lane_right_ = network_.add_lane(section_, -1, LaneType::Driving);
  }

  RoadNetwork network_;
  RoadId road_a_;
  RoadId road_b_;
  LaneSectionId section_;
  LaneId lane_left_;
  LaneId lane_right_;
};

TEST_F(DiagnosticLocatorTest, ResolvesRoadByDocumentIndex) {
  const auto target = resolve_diagnostic_location(network_, "road[0]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_a_);
  EXPECT_FALSE(target->lane.is_valid());
}

TEST_F(DiagnosticLocatorTest, ResolvesLaneByOdrId) {
  const auto target = resolve_diagnostic_location(network_, "road[1]/laneSection[0]/lane[-1]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_b_);
  EXPECT_EQ(target->lane, lane_right_);
}

TEST_F(DiagnosticLocatorTest, DeeperSuffixKeepsLaneMatch) {
  const auto target =
      resolve_diagnostic_location(network_, "road[1]/laneSection[0]/lane[1]/width[2]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_b_);
  EXPECT_EQ(target->lane, lane_left_);
}

TEST_F(DiagnosticLocatorTest, UnknownSubPathFallsBackToRoad) {
  const auto target = resolve_diagnostic_location(network_, "road[0]/planView/geometry[3]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_a_);
  EXPECT_FALSE(target->lane.is_valid());
}

TEST_F(DiagnosticLocatorTest, OutOfRangeSectionKeepsRoadMatch) {
  const auto target = resolve_diagnostic_location(network_, "road[1]/laneSection[7]/lane[-1]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_b_);
  EXPECT_FALSE(target->lane.is_valid());
}

TEST_F(DiagnosticLocatorTest, UnknownLaneIdKeepsRoadMatch) {
  const auto target = resolve_diagnostic_location(network_, "road[1]/laneSection[0]/lane[9]");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->road, road_b_);
  EXPECT_FALSE(target->lane.is_valid());
}

TEST_F(DiagnosticLocatorTest, RejectsNonMatchingLocations) {
  EXPECT_FALSE(resolve_diagnostic_location(network_, "").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "junction[0]").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "road[2]").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "road[-1]").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "road[]").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "road[abc]").has_value());
  EXPECT_FALSE(resolve_diagnostic_location(network_, "roadway[0]").has_value());
}

TEST(ExtractRuleId, FindsAsamRuleInsideMessage) {
  EXPECT_EQ(extract_rule_id("violates asam.net:xodr:1.4.0:ids.id_unique_in_class."),
            "asam.net:xodr:1.4.0:ids.id_unique_in_class");
  EXPECT_EQ(extract_rule_id("asam.net:xodr:1.9.0:road.length_positive required"),
            "asam.net:xodr:1.9.0:road.length_positive");
}

TEST(ExtractRuleId, EmptyWhenNoRuleCited) {
  EXPECT_TRUE(extract_rule_id("road has no usable planView geometry").empty());
  EXPECT_TRUE(extract_rule_id("").empty());
}

} // namespace
} // namespace roadmaker::editor
