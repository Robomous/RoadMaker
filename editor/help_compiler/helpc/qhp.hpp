#pragma once

// Emits the Qt Help project (.qhp) and collection project (.qhcp) that
// qhelpgenerator compiles into the shipped .qch/.qhc. Qt-free.

#include <string>

#include "helpc/toc.hpp"

namespace roadmaker::helpc {

struct QhpOptions {
  std::string namespace_ = "ai.robomous.roadmaker";
  std::string folder = "doc";
  std::string version = "0.1.0";
  std::string qch_file = "roadmaker.qch"; ///< the .qch the collection registers
};

/// Escape the five XML metacharacters (`&` first).
[[nodiscard]] std::string xml_escape(const std::string& text);

/// The .qhp: a TOC tree (index → pages, tutorials nested) plus exactly one
/// `<keyword>` per page — `id` is the slug (help-s2 contract) — and the file
/// manifest qhelpgenerator bundles.
[[nodiscard]] std::string build_qhp(const Toc& toc, const QhpOptions& opts);

/// The .qhcp collection project: registers the .qch produced from the .qhp.
[[nodiscard]] std::string build_qhcp(const QhpOptions& opts);

} // namespace roadmaker::helpc
