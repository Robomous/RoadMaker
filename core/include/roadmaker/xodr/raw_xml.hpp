// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace roadmaker {

/// Verbatim passthrough for the parts of an OpenDRIVE element RoadMaker does
/// not model semantically (the "Preserved" tier, docs/design/m3a/01 §5): the
/// parser never silently drops input, so unknown attributes and unmodeled
/// child elements are captured here at parse time, in document order, and the
/// writer re-emits them after the modeled content.
struct RawXml {
  /// Attributes the owning struct has no typed field for, as (name, value)
  /// in document order. Also used for attribute VALUES that cannot be
  /// represented faithfully by the typed field (e.g. an e_objectType string
  /// outside the modeled enum), so writing the enum back never loses the
  /// original spelling.
  std::vector<std::pair<std::string, std::string>> attributes;

  /// Serialized unmodeled child elements (e.g. <skeleton>, <markings>,
  /// <material>, <userData>), each a self-contained XML fragment, in
  /// document order.
  std::vector<std::string> children;

  [[nodiscard]] bool empty() const { return attributes.empty() && children.empty(); }

  friend bool operator==(const RawXml&, const RawXml&) = default;
};

} // namespace roadmaker
