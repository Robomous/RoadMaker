// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "document/marking_item.hpp"

namespace roadmaker::editor {

std::optional<RoadMarkType> mark_type_from_string(const QString& type) {
  if (type == QStringLiteral("solid")) {
    return RoadMarkType::Solid;
  }
  if (type == QStringLiteral("broken")) {
    return RoadMarkType::Broken;
  }
  if (type == QStringLiteral("solid_solid")) {
    return RoadMarkType::SolidSolid;
  }
  if (type == QStringLiteral("solid_broken")) {
    return RoadMarkType::SolidBroken;
  }
  if (type == QStringLiteral("broken_solid")) {
    return RoadMarkType::BrokenSolid;
  }
  if (type == QStringLiteral("broken_broken")) {
    return RoadMarkType::BrokenBroken;
  }
  return std::nullopt;
}

std::optional<RoadMarkColor> mark_color_from_string(const QString& color) {
  if (color.isEmpty() || color == QStringLiteral("standard")) {
    return RoadMarkColor::Standard;
  }
  if (color == QStringLiteral("white")) {
    return RoadMarkColor::White;
  }
  if (color == QStringLiteral("yellow")) {
    return RoadMarkColor::Yellow;
  }
  if (color == QStringLiteral("red")) {
    return RoadMarkColor::Red;
  }
  if (color == QStringLiteral("blue")) {
    return RoadMarkColor::Blue;
  }
  if (color == QStringLiteral("green")) {
    return RoadMarkColor::Green;
  }
  if (color == QStringLiteral("orange")) {
    return RoadMarkColor::Orange;
  }
  return std::nullopt;
}

std::optional<RoadMark> mark_from_item(const LibraryItem& item) {
  const auto type = mark_type_from_string(item.mark_type);
  const auto color = mark_color_from_string(item.mark_color);
  if (!type.has_value() || !color.has_value()) {
    return std::nullopt;
  }
  RoadMark mark;
  mark.s_offset = 0.0;
  mark.type = *type;
  mark.width = item.mark_width;
  mark.color = *color;
  return mark;
}

} // namespace roadmaker::editor
