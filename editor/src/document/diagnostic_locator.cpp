#include "document/diagnostic_locator.hpp"

#include <cctype>
#include <charconv>

namespace roadmaker::editor {

namespace {

/// Consumes "<name>[<int>]" from the front of `text`; on match, advances
/// `text` past the closing bracket and returns the integer.
std::optional<int> consume_indexed(std::string_view& text, std::string_view name) {
  if (!text.starts_with(name) || text.size() <= name.size() || text[name.size()] != '[') {
    return std::nullopt;
  }
  const std::size_t open = name.size() + 1;
  const std::size_t close = text.find(']', open);
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  int value = 0;
  const auto [ptr, ec] = std::from_chars(text.data() + open, text.data() + close, value);
  if (ec != std::errc{} || ptr != text.data() + close) {
    return std::nullopt;
  }
  text.remove_prefix(close + 1);
  return value;
}

/// Consumes a single '/' separator if present.
bool consume_separator(std::string_view& text) {
  if (text.starts_with('/')) {
    text.remove_prefix(1);
    return true;
  }
  return false;
}

RoadId road_by_document_index(const RoadNetwork& network, int index) {
  RoadId found;
  int current = 0;
  network.for_each_road([&](RoadId id, const Road&) {
    if (current++ == index) {
      found = id;
    }
  });
  return found;
}

} // namespace

std::optional<DiagnosticTarget> resolve_diagnostic_location(const RoadNetwork& network,
                                                            std::string_view location) {
  const auto road_index = consume_indexed(location, "road");
  if (!road_index || *road_index < 0) {
    return std::nullopt;
  }
  const RoadId road_id = road_by_document_index(network, *road_index);
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return std::nullopt;
  }

  DiagnosticTarget target{.road = road_id, .lane = {}};
  if (!consume_separator(location)) {
    return target; // "road[N]" alone, or trailing garbage — road is enough
  }

  const auto section_index = consume_indexed(location, "laneSection");
  if (!section_index || *section_index < 0 ||
      static_cast<std::size_t>(*section_index) >= road->sections.size()) {
    return target; // unknown sub-path (planView/...): keep the road match
  }
  const LaneSection* section =
      network.lane_section(road->sections[static_cast<std::size_t>(*section_index)]);
  if (section == nullptr || !consume_separator(location)) {
    return target;
  }

  const auto odr_lane_id = consume_indexed(location, "lane");
  if (!odr_lane_id) {
    return target;
  }
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane != nullptr && lane->odr_id == *odr_lane_id) {
      target.lane = lane_id;
      break;
    }
  }
  return target;
}

std::string_view extract_rule_id(std::string_view message) {
  constexpr std::string_view kPrefix = "asam.net:";
  const std::size_t start = message.find(kPrefix);
  if (start == std::string_view::npos) {
    return {};
  }
  std::size_t end = start + kPrefix.size();
  auto is_rule_char = [](char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '.' || c == ':' || c == '_' ||
           c == '-';
  };
  while (end < message.size() && is_rule_char(message[end])) {
    ++end;
  }
  // Trim trailing punctuation that belongs to the sentence, not the id.
  while (end > start && (message[end - 1] == '.' || message[end - 1] == ':')) {
    --end;
  }
  return message.substr(start, end - start);
}

} // namespace roadmaker::editor
