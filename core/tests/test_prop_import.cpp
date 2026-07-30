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

// The glTF/GLB prop importer (p6-s8, #322 · ADR-0013).
//
// Fixtures are committed files read off disk, written by
// scripts/gen_gltf_fixtures.py — a reader checked only against bytes the same
// test just built agrees with itself about everything, including its mistakes.

#include "roadmaker/assets/prop_import.hpp"
#include "roadmaker/io/gltf_exporter.hpp"
#include "roadmaker/mesh/mesh.hpp"

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using roadmaker::Severity;
using roadmaker::props::import_prop_model;
using roadmaker::props::PropImportOptions;
using roadmaker::props::PropImportResult;
using roadmaker::props::PropModel;
using roadmaker::props::PropPart;

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(RM_GLTF_FIXTURES_DIR) / name;
}

PropImportResult import_or_fail(const char* name, const PropImportOptions& options = {}) {
  auto result = import_prop_model(fixture(name), "imported", options);
  if (!result.has_value()) {
    throw std::runtime_error(std::string(name) + ": " + result.error().message);
  }
  return std::move(*result);
}

struct Bounds {
  std::array<double, 3> lo{};
  std::array<double, 3> hi{};
};

Bounds bounds_of(const std::vector<PropPart>& parts) {
  Bounds b{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
  for (const PropPart& part : parts) {
    for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
      for (std::size_t c = 0; c < 3; ++c) {
        b.lo[c] = std::min(b.lo[c], part.positions[i + c]);
        b.hi[c] = std::max(b.hi[c], part.positions[i + c]);
      }
    }
  }
  return b;
}

bool any_message_contains(const std::vector<roadmaker::Diagnostic>& diagnostics,
                          std::string_view needle) {
  return std::any_of(
      diagnostics.begin(), diagnostics.end(), [needle](const roadmaker::Diagnostic& d) {
        return d.message.find(needle) != std::string::npos;
      });
}

// The box every well-formed fixture carries: 1 x 2 x 3 in glTF axes, so 1 wide,
// 3 deep and 2 TALL once Y-up becomes Z-up.
constexpr double kBoxHeight = 2.0;
constexpr double kBoxRadius = 1.5811388300841898; // 0.5 * sqrt(1 + 9)

} // namespace

// --------------------------------------------------------------------------- //
// The closed format list
// --------------------------------------------------------------------------- //

TEST(PropImportFormats, ExtensionPredicateMatchesTheClosedList) {
  EXPECT_TRUE(roadmaker::props::is_prop_model_extension("chair.glb"));
  EXPECT_TRUE(roadmaker::props::is_prop_model_extension("chair.gltf"));
  // Case is not the user's problem.
  EXPECT_TRUE(roadmaker::props::is_prop_model_extension("CHAIR.GLB"));
  EXPECT_TRUE(roadmaker::props::is_prop_model_extension("Chair.Gltf"));
  // Everything ADR-0013 refuses.
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair.obj"));
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair.fbx"));
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair.usda"));
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair.usdc"));
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair.png"));
  EXPECT_FALSE(roadmaker::props::is_prop_model_extension("chair"));
}

TEST(PropImportFormats, ARefusedFormatNamesItselfAndTheIssueThatWouldLiftIt) {
  const auto result = import_prop_model(fixture("chair.obj"), "chair");
  ASSERT_FALSE(result.has_value());
  // The refusal has to be actionable: what is supported, and where OBJ is
  // tracked. This is the contract ADR-0013 states for every refusal.
  EXPECT_NE(result.error().message.find(".gltf"), std::string::npos);
  EXPECT_NE(result.error().message.find(".glb"), std::string::npos);
  EXPECT_NE(result.error().message.find("#511"), std::string::npos);
}

TEST(PropImportFormats, AnEmptyIdIsRefusedBeforeAnythingIsRead) {
  const auto result = import_prop_model(fixture("textured_box.glb"), "");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}

TEST(PropImportFormats, AMissingFileIsNotFound) {
  const auto result = import_prop_model(fixture("does_not_exist.glb"), "x");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::FileNotFound);
}

// --------------------------------------------------------------------------- //
// The frame conversion — the assertion that pins Y-up -> Z-up
// --------------------------------------------------------------------------- //

// WHY A HAND-WRITTEN FIXTURE AND NOT ONLY THE ROUND TRIP BELOW: every bundled
// prop model is symmetric about its y axis, and y is the axis this conversion
// negates. So an export/import round trip cannot see a sign error there —
// reflecting a y-symmetric model leaves it unchanged. These four vertices are
// asymmetric on all three axes and their expected values come from the spec's
// (x, y, z) -> (x, -z, y), computed by hand.
TEST(PropImportFrame, KernelCoordinatesFollowTheSpecInEveryAxis) {
  const PropImportResult imported = import_or_fail("asymmetric_wedge.glb");
  ASSERT_EQ(imported.model.parts.size(), 1U);

  // glTF: (0,0,0) (3,0,0) (0,5,0) (0,0,7)
  //   -> kernel (x, -z, y): (0,0,0) (3,0,0) (0,0,5) (0,-7,0)
  //   -> extents x[0,3] y[-7,0] z[0,5]; shift x-1.5, y+3.5, z 0
  const std::vector<std::array<double, 3>> expected = {
      {-1.5, 3.5, 0.0}, {1.5, 3.5, 0.0}, {-1.5, 3.5, 5.0}, {-1.5, -3.5, 0.0}};

  const PropPart& part = imported.model.parts.front();
  ASSERT_EQ(part.positions.size(), expected.size() * 3);
  for (std::size_t v = 0; v < expected.size(); ++v) {
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_NEAR(part.positions[(v * 3) + c], expected[v][c], 1e-6)
          << "vertex " << v << " component " << c;
    }
  }
  EXPECT_NEAR(imported.model.height, 5.0, 1e-9);
  EXPECT_NEAR(imported.model.radius, 0.5 * std::sqrt(58.0), 1e-9);
}

TEST(PropImportFrame, TheModelIsReseatedOnItsBaseAndCentredHorizontally) {
  const PropImportResult imported = import_or_fail("textured_box.glb");
  const Bounds b = bounds_of(imported.model.parts);
  // The PropPart contract: z = 0 sits on the surface, origin at the base centre.
  EXPECT_NEAR(b.lo[2], 0.0, 1e-9);
  EXPECT_NEAR(b.lo[0] + b.hi[0], 0.0, 1e-9);
  EXPECT_NEAR(b.lo[1] + b.hi[1], 0.0, 1e-9);
  // The height is the glTF Y extent (2), not the x (1) or z (3) one — which is
  // the whole point of the conversion.
  EXPECT_NEAR(imported.model.height, kBoxHeight, 1e-6);
  EXPECT_NEAR(imported.model.radius, kBoxRadius, 1e-6);
  // Both must be positive or the renderer cannot frame it and per-instance
  // sizing divides by zero.
  EXPECT_GT(imported.model.height, 0.0);
  EXPECT_GT(imported.model.radius, 0.0);
}

TEST(PropImportFrame, NestedNodeTransformsComposeToTheSameModel) {
  // nested_box.glb is a UNIT box under a translating root and a scaling leaf.
  // If composition order or the matrix layout were wrong, its extents would not
  // match the fixture that states the same box directly.
  const PropImportResult direct = import_or_fail("factor_box.glb");
  const PropImportResult nested = import_or_fail("nested_box.glb");
  EXPECT_NEAR(nested.model.height, direct.model.height, 1e-6);
  EXPECT_NEAR(nested.model.radius, direct.model.radius, 1e-6);
  const Bounds a = bounds_of(direct.model.parts);
  const Bounds b = bounds_of(nested.model.parts);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_NEAR(a.lo[c], b.lo[c], 1e-6) << "axis " << c;
    EXPECT_NEAR(a.hi[c], b.hi[c], 1e-6) << "axis " << c;
  }
}

// --------------------------------------------------------------------------- //
// Colour: the flatten decision (ADR-0013)
// --------------------------------------------------------------------------- //

TEST(PropImportColor, AFactorOnlyMaterialKeepsItsColourExactly) {
  const PropImportResult imported = import_or_fail("factor_box.glb");
  ASSERT_EQ(imported.model.parts.size(), 1U);
  const std::array<float, 3>& color = imported.model.parts.front().color;
  EXPECT_NEAR(color[0], 0.25F, 1e-6);
  EXPECT_NEAR(color[1], 0.5F, 1e-6);
  EXPECT_NEAR(color[2], 0.75F, 1e-6);
  // Nothing was flattened, so nothing may claim it was.
  EXPECT_FALSE(any_message_contains(imported.diagnostics, "flattened"));
}

TEST(PropImportColor, ATexturedMaterialFlattensToItsAverageLinearColourAndCitesTheFollowUp) {
  const PropImportResult imported = import_or_fail("textured_box.glb");
  ASSERT_EQ(imported.model.parts.size(), 1U);
  const std::array<float, 3>& color = imported.model.parts.front().color;

  // The fixture's texture is a flat sRGB (204, 102, 51) = (0.8, 0.4, 0.2), and
  // PropPart::color is documented LINEAR. These are the sRGB EOTF applied to
  // each channel, from the glTF spec — not numbers this reader produced.
  const auto srgb_to_linear = [](double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  EXPECT_NEAR(color[0], static_cast<float>(srgb_to_linear(0.8)), 1e-4);
  EXPECT_NEAR(color[1], static_cast<float>(srgb_to_linear(0.4)), 1e-4);
  EXPECT_NEAR(color[2], static_cast<float>(srgb_to_linear(0.2)), 1e-4);

  // NON-VACUITY GUARD. Without the transfer function the average of the encoded
  // bytes would be (0.8, 0.4, 0.2) — every channel visibly brighter. If this
  // ever passes, the assertions above have stopped testing the decode.
  EXPECT_LT(color[0], 0.75F);
  EXPECT_LT(color[1], 0.30F);
  EXPECT_LT(color[2], 0.10F);

  // The user is told what they lost and where it is tracked.
  ASSERT_TRUE(any_message_contains(imported.diagnostics, "flattened"));
  EXPECT_TRUE(any_message_contains(imported.diagnostics, "#507"));
  EXPECT_TRUE(any_message_contains(imported.diagnostics, "crate_paint"));
}

TEST(PropImportColor, AnImageBesideTheGltfIsReadAndAveragedTheSameWay) {
  // TINYGLTF_NO_EXTERNAL_IMAGE means tinygltf keeps the uri and loads nothing,
  // so this path is entirely the importer's own.
  const PropImportResult external = import_or_fail("external_box.gltf");
  const PropImportResult embedded = import_or_fail("textured_box.glb");
  ASSERT_EQ(external.model.parts.size(), 1U);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_NEAR(external.model.parts.front().color[c], embedded.model.parts.front().color[c], 1e-6)
        << "channel " << c;
  }
}

TEST(PropImportColor, AnImageUriEscapingTheModelDirectoryIsRefusedByName) {
  // Nothing but this check stands between a hostile .gltf and the filesystem.
  const PropImportResult imported = import_or_fail("path_traversal.gltf");
  EXPECT_TRUE(any_message_contains(imported.diagnostics, "outside the model's own directory"));
  // The geometry still imports: a refused texture is not a refused model.
  ASSERT_EQ(imported.model.parts.size(), 1U);
  EXPECT_NEAR(imported.model.height, kBoxHeight, 1e-6);
  // And nothing was flattened, because nothing was read.
  EXPECT_FALSE(any_message_contains(imported.diagnostics, "flattened"));
}

// --------------------------------------------------------------------------- //
// Attributes the file may omit
// --------------------------------------------------------------------------- //

TEST(PropImportAttributes, MissingNormalsAreComputedAsUnitVectors) {
  const PropImportResult imported = import_or_fail("no_normals_box.glb");
  ASSERT_EQ(imported.model.parts.size(), 1U);
  const PropPart& part = imported.model.parts.front();
  ASSERT_EQ(part.normals.size(), part.positions.size());
  ASSERT_FALSE(part.normals.empty());
  for (std::size_t i = 0; i + 2 < part.normals.size(); i += 3) {
    const double len = std::sqrt((part.normals[i] * part.normals[i]) +
                                 (part.normals[i + 1] * part.normals[i + 1]) +
                                 (part.normals[i + 2] * part.normals[i + 2]));
    // A zero normal renders black, which is the failure this guards.
    EXPECT_NEAR(len, 1.0, 1e-9) << "normal " << (i / 3);
  }
}

TEST(PropImportAttributes, ANonIndexedPrimitiveGetsItsIndicesSynthesised) {
  const PropImportResult imported = import_or_fail("nonindexed_box.glb");
  ASSERT_EQ(imported.model.parts.size(), 1U);
  const PropPart& part = imported.model.parts.front();
  EXPECT_EQ(part.indices.size(), part.positions.size() / 3);
  EXPECT_EQ(part.indices.size() % 3, 0U);
  EXPECT_NEAR(imported.model.height, kBoxHeight, 1e-6);
}

TEST(PropImportAttributes, NonTrianglePrimitivesAreSkippedAndSaidSo) {
  const PropImportResult imported = import_or_fail("two_parts_and_lines.glb");
  // Three primitives in, two parts out — the LINES one is not a surface.
  EXPECT_EQ(imported.model.parts.size(), 2U);
  EXPECT_TRUE(any_message_contains(imported.diagnostics, "not TRIANGLES"));
  // Part names become exported material names, so neither may be empty.
  for (const PropPart& part : imported.model.parts) {
    EXPECT_FALSE(part.name.empty());
  }
  EXPECT_EQ(imported.model.parts[0].name, "first");
  EXPECT_EQ(imported.model.parts[1].name, "second");
}

// --------------------------------------------------------------------------- //
// Stated budgets
// --------------------------------------------------------------------------- //

TEST(PropImportBudgets, EachLimitIsRefusedWithItsNumberInTheMessage) {
  {
    PropImportOptions options;
    options.max_parts = 1;
    const auto result = import_prop_model(fixture("two_parts_and_lines.glb"), "x", options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("1"), std::string::npos);
    EXPECT_NE(result.error().message.find("primitives"), std::string::npos);
  }
  {
    PropImportOptions options;
    options.max_triangles = 2;
    const auto result = import_prop_model(fixture("textured_box.glb"), "x", options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("triangles"), std::string::npos);
  }
  {
    PropImportOptions options;
    options.max_vertices = 3;
    const auto result = import_prop_model(fixture("textured_box.glb"), "x", options);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("vertices"), std::string::npos);
  }
}

TEST(PropImportBudgets, TheDefaultsAreGenerousEnoughForTheFixtures) {
  // A budget that refuses ordinary input is a bug, not a safeguard.
  EXPECT_NO_THROW(import_or_fail("textured_box.glb"));
  EXPECT_GT(roadmaker::props::kMaxPropTriangles, 1000U);
  EXPECT_GT(roadmaker::props::kMaxPropVertices, 1000U);
}

// --------------------------------------------------------------------------- //
// Malformed input: fuzz-adjacent, per #322's acceptance
// --------------------------------------------------------------------------- //

// ★ EACH FIXTURE ASSERTS THE REASON IT IS REFUSED FOR, not merely that it is
// refused. This suite began as "every malformed file returns an error", and two
// of its cases were passing for reasons that had nothing to do with what they
// were meant to cover: `cyclic_nodes.gltf` was rejected by tinygltf for a
// missing buffer uri before the cycle guard ran, and `nan_position.glb` was
// rejected by the JSON parser because a NaN had leaked into the accessor's
// `min`. Both guards were therefore untested while the suite was green. A
// refusal is only covered when the test names WHY.
struct MalformedCase {
  const char* file;
  const char* expected_reason;
};

class PropImportMalformed : public testing::TestWithParam<MalformedCase> {};

TEST_P(PropImportMalformed, IsRefusedForTheStatedReasonAndNeverCrashes) {
  const auto result = import_prop_model(fixture(GetParam().file), "malformed");
  ASSERT_FALSE(result.has_value()) << GetParam().file << " was accepted";
  // A refusal with no explanation is a crash the user has to guess about.
  EXPECT_NE(result.error().message.find(GetParam().expected_reason), std::string::npos)
      << GetParam().file << " was refused, but not for the reason this case covers.\n"
      << "  expected to mention: " << GetParam().expected_reason << "\n"
      << "  actual message:      " << result.error().message;
}

INSTANTIATE_TEST_SUITE_P(Files,
                         PropImportMalformed,
                         testing::Values(
                             // tinygltf's own container checks, forwarded rather than swallowed.
                             MalformedCase{"truncated.glb", "Invalid glTF binary"},
                             MalformedCase{"bad_magic.glb", "Invalid magic"},
                             MalformedCase{"lying_chunk_length.glb", "Invalid glTF binary"},
                             // An index past the vertex array: caught before it is dereferenced, so
                             // the primitive is skipped and nothing usable is left. ASan in CI is
                             // the other half of that claim.
                             MalformedCase{"index_out_of_range.glb", "points past the"},
                             // A NaN vertex, which survives a float round-trip and so is a file a
                             // broken exporter really writes.
                             MalformedCase{"nan_position.glb", "non-finite"},
                             // The spec says the node graph is a forest; a reader that believes it
                             // recurses until the stack ends.
                             MalformedCase{"cyclic_nodes.gltf", "cyclic"},
                             MalformedCase{"no_geometry.gltf", "no triangle geometry"},
                             // Zero height would make per-instance sizing divide by it.
                             MalformedCase{"zero_height.glb", "no measurable size"}),
                         [](const testing::TestParamInfo<MalformedCase>& info) {
                           std::string name = info.param.file;
                           std::replace(name.begin(), name.end(), '.', '_');
                           return name;
                         });

TEST(PropImportMalformedDetail, AFailedImportCarriesTheReasonItFailed) {
  // import_prop_model returns a result OR an error, so on failure the
  // diagnostics are discarded. "no triangle geometry" alone would tell a user
  // nothing about the NaN that actually emptied the file, so the first warning
  // is folded into the error message.
  const auto result = import_prop_model(fixture("nan_position.glb"), "x");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("every primitive was skipped"), std::string::npos)
      << result.error().message;
  EXPECT_NE(result.error().message.find("non-finite"), std::string::npos) << result.error().message;
}

TEST(PropImportMalformedDetail, ARecursionLimitBoundsADeepNodeChain) {
  // A cycle is not the only unbounded walk: a few hundred kilobytes of JSON can
  // state a chain far deeper than the stack, and that is not a cycle so the
  // guard above cannot catch it. Built here rather than committed because the
  // interesting part is its size, not its bytes.
  const std::filesystem::path deep = std::filesystem::temp_directory_path() / "rm_deep_nodes.gltf";
  {
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)";
    json += R"("nodes":[)";
    constexpr int kChain = 4000;
    for (int i = 0; i < kChain; ++i) {
      json += i + 1 < kChain ? fmt::format(R"({{"children":[{}]}},)", i + 1) : R"({})";
    }
    json += "]}";
    std::ofstream out(deep);
    out << json;
  }
  const auto result = import_prop_model(deep, "deep");
  std::remove(deep.string().c_str());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("deeper than"), std::string::npos)
      << result.error().message;
}

// --------------------------------------------------------------------------- //
// The round-trip oracle: the importer against the EXPORTER, not against
// arithmetic done twice
// --------------------------------------------------------------------------- //

TEST(PropImportRoundTrip, ABundledModelSurvivesExportAndReimport) {
  // streetlight_single is asymmetric in x and z (the arm), so a transposed or
  // negated axis on either shows up here. The y sign is pinned by
  // PropImportFrame.KernelCoordinatesFollowTheSpecInEveryAxis instead, because
  // every bundled model is y-symmetric — see the note there.
  const PropModel* original = roadmaker::props::model("streetlight_single");
  ASSERT_NE(original, nullptr);

  // NON-VACUITY GUARD for the paragraph above: if this model ever became
  // symmetric on x, this test would stop pinning anything about that axis.
  {
    const Bounds b = bounds_of(original->parts);
    EXPECT_GT(std::abs(b.hi[0] + b.lo[0]), 1e-6)
        << "streetlight_single is no longer x-asymmetric; this oracle has gone vacuous";
  }

  roadmaker::NetworkMesh mesh;
  mesh.objects.push_back(roadmaker::ObjectInstance{.object = {},
                                                   .road = {},
                                                   .model_id = "streetlight_single",
                                                   .position = {0.0, 0.0, 0.0},
                                                   .heading = 0.0,
                                                   .scale = 1.0});
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "rm_prop_roundtrip.glb";
  const auto exported = roadmaker::export_glb(mesh, out);
  ASSERT_TRUE(exported.has_value()) << exported.error().message;

  auto reimported = import_prop_model(out, "streetlight_single_reimported");
  std::remove(out.string().c_str());
  ASSERT_TRUE(reimported.has_value()) << reimported.error().message;

  EXPECT_EQ(reimported->model.parts.size(), original->parts.size());

  std::size_t original_indices = 0;
  std::size_t reimported_indices = 0;
  for (const PropPart& part : original->parts) {
    original_indices += part.indices.size();
  }
  for (const PropPart& part : reimported->model.parts) {
    reimported_indices += part.indices.size();
  }
  EXPECT_EQ(reimported_indices, original_indices);

  // Positions are compared anchored to each model's own bounding-box minimum,
  // which makes the comparison independent of the re-seating the importer does
  // and still sensitive to a permuted or negated axis. Tolerance is float32's,
  // because the exporter writes floats.
  const auto anchored = [](const std::vector<PropPart>& parts) {
    const Bounds b = bounds_of(parts);
    std::vector<std::array<double, 3>> out;
    for (const PropPart& part : parts) {
      for (std::size_t i = 0; i + 2 < part.positions.size(); i += 3) {
        out.push_back({part.positions[i] - b.lo[0],
                       part.positions[i + 1] - b.lo[1],
                       part.positions[i + 2] - b.lo[2]});
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  const std::vector<std::array<double, 3>> before = anchored(original->parts);
  const std::vector<std::array<double, 3>> after = anchored(reimported->model.parts);
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t v = 0; v < before.size(); ++v) {
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_NEAR(after[v][c], before[v][c], 1e-4) << "vertex " << v << " component " << c;
    }
  }

  EXPECT_NEAR(reimported->model.height, original->height, 1e-4);
}
