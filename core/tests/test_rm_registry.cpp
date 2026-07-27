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

// The rm: userData registry conformance gate (fmt-s2, #326). ADR-0008's policy
// is that every `rm:` code the writer emits ships with a parser, a fuzz-corpus
// sample, a round-trip test, and a row in the ADR's registry block. The code
// side of the registry is roadmaker/xodr/rm_codes.hpp; these tests keep the
// committed repo in step with it — the test_defaults_registry mechanism (#413)
// extended from docs to sources: a new emission landing without its coverage
// fails CI here, naming the missing artifact, not in review.
//
// The scans read the committed files off disk (RM_CORE_SRC_DIR /
// RM_CORE_TESTS_DIR / RM_FUZZ_CORPUS_DIR / RM_DOCS_DIR are compile
// definitions). They are STRICT about quoted rm: strings: a comment quoting an
// unregistered code (e.g. `code="rm:something"`) fails the writer gate too —
// deliberate, comments must not describe codes that do not exist.

#include "roadmaker/xodr/rm_codes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roadmaker {
namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "missing " << path.string();
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::filesystem::path xodr_src_dir() {
  return std::filesystem::path(RM_CORE_SRC_DIR) / "xodr";
}

/// Every `"rm:..."` STRING LITERAL in `text` whose body is a plausible code
/// (letters, digits, '_', '.'), e.g. `set_value("rm:arms")` or a comment's
/// `code="rm:arms"`. Attribute-value material ids (rm:asphalt, ...) never
/// appear as quoted literals in the reader/writer — they flow from catalogues —
/// so no allowlist is needed; if one ever does, register or rename it.
std::set<std::string> quoted_rm_strings(const std::string& text) {
  std::set<std::string> found;
  for (std::size_t at = text.find("\"rm:"); at != std::string::npos;
       at = text.find("\"rm:", at + 1)) {
    const std::size_t begin = at + 1;
    const std::size_t end = text.find('"', begin);
    if (end == std::string::npos) {
      break;
    }
    const std::string code = text.substr(begin, end - begin);
    const bool plausible =
        code.size() > 3 && std::all_of(code.begin() + 3, code.end(), [](unsigned char c) {
          return std::isalnum(c) != 0 || c == '_' || c == '.';
        });
    if (plausible) {
      found.insert(code);
    }
  }
  return found;
}

/// `code` used as a full quoted-or-XML token: the code followed by a closing
/// double quote. This is what distinguishes `rm:signal` from its prefix-sibling
/// `rm:signalmount` in both C++ literals ("rm:signal") and corpus XML
/// (code="rm:signal").
bool contains_code_token(const std::string& text, std::string_view code) {
  return text.find(std::string(code) + "\"") != std::string::npos;
}

} // namespace

// The ADR's registry block is generated text: exactly one line per scope, in
// kRmCodes order. A code added to rm_codes.hpp without amending the ADR (or
// vice versa) fails here with the correct block in the message, copy-paste
// ready — the rm-defaults table mechanism.
TEST(RmRegistry, AdrRegistryBlockMatchesTheCode) {
  const std::string doc = read_file(std::filesystem::path(RM_DOCS_DIR) / "decisions" /
                                    "0008-persistence-layers-asam-first.md");
  ASSERT_NE(doc.find("<!-- rm-registry:begin"), std::string::npos);
  ASSERT_NE(doc.find("<!-- rm-registry:end -->"), std::string::npos);

  const std::pair<RmCodeScope, std::string_view> scopes[] = {
      {RmCodeScope::Road, "road"},
      {RmCodeScope::Object, "object"},
      {RmCodeScope::Junction, "junction"},
      {RmCodeScope::Root, "root"},
  };
  std::string rendered;
  for (const auto& [scope, label] : scopes) {
    rendered += "- ";
    rendered += label;
    rendered += ":";
    bool first = true;
    for (const RmCode& entry : kRmCodes) {
      if (entry.scope != scope) {
        continue;
      }
      rendered += first ? " `" : ", `";
      rendered += entry.code;
      rendered += "`";
      first = false;
    }
    rendered += "\n";
  }
  EXPECT_NE(doc.find(rendered), std::string::npos)
      << "ADR-0008's rm-registry block is out of step with rm_codes.hpp; it must contain "
         "exactly:\n"
      << rendered;
}

// Writer ⊆ registry AND registry ⊆ writer: every quoted rm: string in
// writer.cpp is a registered code, and every registered code is emitted
// somewhere. A new `<userData code="rm:...">` emission cannot land without a
// registry row (which then demands parser, corpus and round-trip coverage via
// the tests below).
TEST(RmRegistry, WriterEmissionsAndRegistryAgree) {
  const std::string writer = read_file(xodr_src_dir() / "writer.cpp");
  for (const std::string& code : quoted_rm_strings(writer)) {
    EXPECT_TRUE(is_registered_rm_code(code))
        << "writer.cpp mentions '" << code
        << "', which is not in roadmaker/xodr/rm_codes.hpp — register it (and give it a "
           "parser, a fuzz-corpus sample and a round-trip test) or rename it";
  }
  for (const RmCode& entry : kRmCodes) {
    EXPECT_TRUE(contains_code_token(writer, entry.code))
        << "registered code '" << entry.code << "' has no emission in writer.cpp";
  }
}

// Every registered code has a parse site. The reader may legitimately quote
// unregistered rm: codes in tests-of-malformed-input style comments, so only
// the registry → reader direction is enforced here; the reader's UNKNOWN-code
// behaviour (preserve verbatim + warn) is pinned by test_user_data_preservation.
TEST(RmRegistry, EveryRegisteredCodeHasAParser) {
  const std::string reader = read_file(xodr_src_dir() / "reader.cpp");
  for (const RmCode& entry : kRmCodes) {
    EXPECT_TRUE(contains_code_token(reader, entry.code))
        << "registered code '" << entry.code << "' has no parse site in reader.cpp";
  }
}

// Every registered code appears in at least one fuzz-corpus document, so the
// libFuzzer harness and the corpus-seed gtests both exercise its parse path.
TEST(RmRegistry, EveryRegisteredCodeHasAFuzzCorpusSample) {
  std::vector<std::string> corpus;
  for (const auto& file : std::filesystem::directory_iterator(RM_FUZZ_CORPUS_DIR)) {
    if (file.path().extension() == ".xodr") {
      corpus.push_back(read_file(file.path()));
    }
  }
  ASSERT_FALSE(corpus.empty());
  for (const RmCode& entry : kRmCodes) {
    const bool sampled = std::any_of(corpus.begin(), corpus.end(), [&](const std::string& doc) {
      return contains_code_token(doc, entry.code);
    });
    EXPECT_TRUE(sampled) << "registered code '" << entry.code
                         << "' appears in no core/tests/fuzz/corpus/*.xodr sample";
  }
}

// Every registered code appears in at least one test other than this one — the
// round-trip-coverage proxy. (Existence of the mention is the enforceable part;
// what the mentioning test asserts is review's job.)
TEST(RmRegistry, EveryRegisteredCodeHasATest) {
  std::vector<std::string> tests;
  for (const auto& file : std::filesystem::directory_iterator(RM_CORE_TESTS_DIR)) {
    const std::string name = file.path().filename().string();
    if (file.path().extension() == ".cpp" && name.starts_with("test_") &&
        name != "test_rm_registry.cpp") {
      tests.push_back(read_file(file.path()));
    }
  }
  ASSERT_FALSE(tests.empty());
  for (const RmCode& entry : kRmCodes) {
    const bool covered = std::any_of(tests.begin(), tests.end(), [&](const std::string& doc) {
      return contains_code_token(doc, entry.code);
    });
    EXPECT_TRUE(covered) << "registered code '" << entry.code
                         << "' appears in no core/tests/test_*.cpp";
  }
}

TEST(RmRegistry, RegistrationPredicateIsExact) {
  static_assert(is_registered_rm_code("rm:waypoints"));
  static_assert(is_registered_rm_code("rm:terrain"));
  static_assert(!is_registered_rm_code("rm:demo"));
  static_assert(!is_registered_rm_code("rm:signalx"));
  // A registered code's prefix or extension is NOT registered.
  static_assert(is_registered_rm_code("rm:signal"));
  static_assert(is_registered_rm_code("rm:signalmount"));
  static_assert(!is_registered_rm_code("rm:signalm"));
  EXPECT_FALSE(is_registered_rm_code("vendor:x"));
}

} // namespace roadmaker
