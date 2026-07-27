/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// docs/domain/connection_contract.md is a GOVERNING SPEC DOC (#403): it is the
// only place the continuity guarantees at a joint are defined, and the code is
// its mirror. These tests are what keeps the two in step — the same mechanism
// docs/domain/realism_defaults.md uses (#413). A tolerance changed in one place
// without the other fails CI, not review, because a contract nobody can trust
// to be current is worse than no contract at all.

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/tol.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {
namespace {

std::string committed_contract() {
  const std::filesystem::path page =
      std::filesystem::path(RM_DOCS_DIR) / "domain" / "connection_contract.md";
  std::ifstream file(page);
  EXPECT_TRUE(file.is_open()) << "missing " << page.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

struct NamedTolerance {
  std::string_view name;
  double value;
};

/// Every weld tolerance the contract's table is required to carry. A new
/// tol::kWeld* constant MUST be added here and to the document's table.
constexpr NamedTolerance kWeldTolerances[] = {
    {"tol::kWeldPosition", tol::kWeldPosition},
    {"tol::kWeldHeading", tol::kWeldHeading},
    {"tol::kWeldCurvature", tol::kWeldCurvature},
    {"tol::kWeldElevation", tol::kWeldElevation},
    {"tol::kWeldGrade", tol::kWeldGrade},
};

/// "1e-3" / "5e-3" as the tolerance table renders them — the shortest form that
/// still names the value exactly. Anything not expressible that way would be a
/// tolerance nobody can read, which is itself worth failing on.
std::string rendered(double value) {
  for (int exponent = 0; exponent >= -12; --exponent) {
    for (int mantissa = 1; mantissa <= 9; ++mantissa) {
      const double candidate = static_cast<double>(mantissa) * std::pow(10.0, exponent);
      if (candidate == value) {
        return std::to_string(mantissa) + "e" + (exponent < 0 ? "-" : "+") +
               std::to_string(exponent < 0 ? -exponent : exponent);
      }
    }
  }
  return {};
}

/// Every kernel operation that can outlive a joint. A factory that can move,
/// re-link or unlink a road end MUST be listed here and given a row in the
/// contract's edit-policy table — an operation with an unstated link policy is
/// exactly how the five move gestures came to disagree with each other (#461).
constexpr std::string_view kJointOutlivingOps[] = {
    "close_gap",
    "create_linked_road",
    "extend_road",
    "translate_roads",
    "rotate_road",
    "move_waypoint",
    "insert_waypoint",
    "delete_waypoint",
    "insert_node_at",
    "set_elevation_profile",
    "set_node_elevation",
    "merge_roads",
};

/// The document text between a marker comment and the next heading of any level.
///
/// Searching a SECTION, not the whole document, is the point: a global find is
/// satisfied by a mention anywhere, which is how a swapped tolerance row passed
/// the first version of the test above (#403) and how `contains("0.8")` passed
/// on 0.800000011920929 (#325). Same class of bug, third appearance.
std::string_view section_after(const std::string& doc, std::string_view marker) {
  const std::size_t start = doc.find(marker);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t body = start + marker.size();
  const std::size_t end = doc.find("\n#", body);
  return std::string_view(doc).substr(body, (end == std::string::npos ? doc.size() : end) - body);
}

} // namespace

TEST(ConnectionContract, ToleranceTableMatchesTolHpp) {
  const std::string doc = committed_contract();
  ASSERT_NE(doc.find("<!-- rm-contract: tolerances -->"), std::string::npos)
      << "the tolerance table lost its marker comment";

  for (const NamedTolerance& tolerance : kWeldTolerances) {
    // The value must appear on the constant's OWN ROW, not merely somewhere in
    // the document. A whole-document search is worthless here: kWeldCurvature
    // is 5e-3 and kWeldPosition is 1e-3, so any row could carry any of the
    // other rows' values and a global find would still be satisfied. (Verified
    // by sabotage: swapping one row's value passes a global search.)
    const std::string row_key = "| `" + std::string(tolerance.name) + "` |";
    const std::size_t row = doc.find(row_key);
    ASSERT_NE(row, std::string::npos)
        << tolerance.name << " has no row in the contract's tolerance table";
    const std::size_t row_end = doc.find('\n', row);
    const std::string_view line(doc.data() + row,
                                (row_end == std::string::npos ? doc.size() : row_end) - row);

    // "1e-3" is how the table writes 1e-3; std::to_string would give
    // "0.001000", which the document deliberately does not use.
    const std::string value = rendered(tolerance.value);
    ASSERT_FALSE(value.empty()) << tolerance.name << " is not a single-digit power of ten; "
                                << "give the table an explicit rendering for it";
    EXPECT_NE(line.find(value), std::string::npos)
        << tolerance.name << " is " << value << " in tol.hpp but the contract's row says: " << line;
  }
}

TEST(ConnectionContract, GradeEaseLengthMatchesTheHeader) {
  const std::string doc = committed_contract();
  EXPECT_NE(doc.find("edit::kGradeEaseLength"), std::string::npos)
      << "the contract must name the constant it governs";
  // 20.0 m renders as "**20 m**" in the chain-creation section.
  const std::string value = std::to_string(static_cast<int>(edit::kGradeEaseLength)) + " m";
  EXPECT_EQ(edit::kGradeEaseLength, static_cast<double>(static_cast<int>(edit::kGradeEaseLength)))
      << "a fractional ease length needs an explicit rendering in this test";
  EXPECT_NE(doc.find(value), std::string::npos)
      << "the contract states an ease length of something other than " << value;
}

TEST(ConnectionContract, CitesTheConnectionRulesAndOnlyRealOnes) {
  const std::string doc = committed_contract();
  ASSERT_NE(doc.find("<!-- rm-contract: rules -->"), std::string::npos);
  EXPECT_NE(doc.find(std::string(rules::kLinkEndsCoincide)), std::string::npos);
  EXPECT_NE(doc.find(std::string(rules::kLinkElevationContinuity)), std::string::npos);

  // Every vendor UID the document quotes must be one the code actually emits —
  // a contract citing a rule that does not exist is worse than silence.
  constexpr std::string_view kVendorPrefix = "robomous.ai:rm:";
  const std::array<std::string_view, 3> known{
      rules::kLinkEndsCoincide, rules::kLinkElevationContinuity, rules::kJunctionArmSingleOwner};
  for (std::size_t at = doc.find(kVendorPrefix); at != std::string::npos;
       at = doc.find(kVendorPrefix, at + 1)) {
    const std::size_t end = doc.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.:_", at);
    const std::string_view cited(doc.data() + at,
                                 (end == std::string::npos ? doc.size() : end) - at);
    if (cited == kVendorPrefix) {
      continue; // prose naming the namespace itself, not a citation
    }
    bool recognised = false;
    for (const std::string_view rule : known) {
      recognised = recognised || cited == rule;
    }
    EXPECT_TRUE(recognised) << "the contract cites an unknown vendor rule: '" << cited << "'";
  }
}

TEST(ConnectionContract, GuaranteesTableNamesEveryJoinKind) {
  const std::string doc = committed_contract();
  ASSERT_NE(doc.find("<!-- rm-contract: guarantees -->"), std::string::npos);
  // The four rows the kernel can actually produce. A join kind added to the
  // engine without a row here would ship an unstated guarantee.
  for (const std::string_view kind : {"Pure link", "Connector", "Junction contact", "Merge seam"}) {
    EXPECT_NE(doc.find(std::string(kind)), std::string::npos)
        << kind << " has no row in the guarantees table";
  }
}

TEST(ConnectionContract, EveryJointOutlivingOperationHasALinkPolicy) {
  const std::string doc = committed_contract();
  ASSERT_NE(doc.find("<!-- rm-contract: edits -->"), std::string::npos)
      << "the edit-policy table lost its marker comment";
  const std::string_view table = section_after(doc, "<!-- rm-contract: edits -->");
  ASSERT_FALSE(table.empty());

  // The FIRST cell of every table row — the operation column. Prose in the
  // section below the table, and the policy text in the second column, are both
  // deliberately out of reach: naming an operation is not stating its policy.
  std::vector<std::string_view> operation_cells;
  for (std::size_t at = 0; at < table.size();) {
    const std::size_t line_end = table.find('\n', at);
    const std::string_view line = table.substr(at, line_end - at);
    at = line_end == std::string_view::npos ? table.size() : line_end + 1;
    if (line.empty() || line.front() != '|') {
      continue;
    }
    const std::size_t cell_end = line.find('|', 1);
    if (cell_end != std::string_view::npos) {
      operation_cells.push_back(line.substr(1, cell_end - 1));
    }
  }
  ASSERT_GT(operation_cells.size(), 2U) << "the edit-policy table has no rows";

  for (const std::string_view op : kJointOutlivingOps) {
    const std::string span = "`" + std::string(op) + "`";
    const bool listed = std::ranges::any_of(operation_cells, [&span](std::string_view cell) {
      return cell.find(span) != std::string_view::npos;
    });
    EXPECT_TRUE(listed)
        << op << " can outlive a joint but has no row in the contract's edit-policy table";
  }
}

} // namespace roadmaker
