// Golden-file structural tests for the OpenUSD (.usda) exporter. USDA is ASCII,
// so these assert on the emitted text (upAxis / metersPerUnit / defaultPrim,
// the Road→LaneSection→Lane Xform hierarchy, material bindings, and the
// MaterialBindingAPI apiSchema). Semantic validity against the OpenUSD
// reference implementation is covered by the `usdchecker` step in the dedicated
// RM_BUILD_USD CI job, which runs against the golden file this suite leaves in
// the temp directory.

#include "roadmaker/io/usd_exporter.hpp"
#include "roadmaker/mesh/mesh_builder.hpp"
#include "roadmaker/xodr/reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string slurp(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// Exports a sample to `out_name` in the temp dir and returns the USDA text.
/// The file is intentionally left on disk so the CI `usdchecker` step can
/// validate it against the OpenUSD reference implementation.
std::string export_sample(const char* sample_name, const char* out_name) {
  auto parsed = roadmaker::load_xodr(std::filesystem::path(RM_SAMPLES_DIR) / sample_name);
  if (!parsed) {
    throw std::runtime_error("failed to load sample");
  }
  const auto mesh = roadmaker::build_network_mesh(parsed->network);

  const auto path = std::filesystem::temp_directory_path() / out_name;
  const auto exported = roadmaker::export_usda(mesh, path);
  if (!exported) {
    throw std::runtime_error("export_usda failed: " + exported.error().message);
  }
  return slurp(path);
}

} // namespace

TEST(Usd, StraightRoadStageMetadataAndHierarchy) {
  const std::string usda = export_sample("straight_road.xodr", "rm_straight.usda");

  EXPECT_EQ(usda.rfind("#usda 1.0", 0), 0U); // magic header at byte 0
  EXPECT_NE(usda.find("upAxis = \"Y\""), std::string::npos);
  EXPECT_NE(usda.find("metersPerUnit = 1"), std::string::npos);
  EXPECT_NE(usda.find("defaultPrim = \"World\""), std::string::npos);

  // Road → LaneSection → Lane Xform/Mesh nesting.
  EXPECT_NE(usda.find("def Xform \"World\""), std::string::npos);
  EXPECT_NE(usda.find("def Xform \"lanesection0\""), std::string::npos);
  EXPECT_NE(usda.find("def Mesh "), std::string::npos);

  // Triangle surface, not a subdivision surface.
  EXPECT_NE(usda.find("subdivisionScheme = \"none\""), std::string::npos);
}

TEST(Usd, MaterialsBoundWithApiSchema) {
  const std::string usda = export_sample("straight_road.xodr", "rm_straight_mat.usda");

  // Materials live under a Looks scope and match the glTF material naming.
  EXPECT_NE(usda.find("def Scope \"Looks\""), std::string::npos);
  EXPECT_NE(usda.find("def Material \"lane_"), std::string::npos);
  EXPECT_NE(usda.find("info:id = \"UsdPreviewSurface\""), std::string::npos);

  // Every bound mesh applies MaterialBindingAPI (usdchecker requirement) and
  // carries a relationship into /Looks.
  EXPECT_NE(usda.find("prepend apiSchemas = [\"MaterialBindingAPI\"]"), std::string::npos);
  EXPECT_NE(usda.find("rel material:binding = </Looks/"), std::string::npos);
}

TEST(Usd, TJunctionExportsFloorSurfaceAndMaterial) {
  // This golden file is the one the CI usdchecker step validates.
  const std::string usda = export_sample("t_junction.xodr", "rm_usd_golden.usda");

  // The floor is one continuous asphalt with the roads feeding it: it binds
  // the driving-lane material (io_common::lane_material_name spelling,
  // "lane_<enum>"), and the legacy junction-debug material never reappears
  // (tee visual finding, follow-up to issue #103).
  const std::string driving_material =
      "lane_" + std::to_string(static_cast<int>(roadmaker::LaneType::Driving));
  EXPECT_NE(usda.find("def Mesh \"junction_"), std::string::npos);
  EXPECT_NE(usda.find("def Material \"" + driving_material + "\""), std::string::npos);
  EXPECT_EQ(usda.find("junction_floor"), std::string::npos);
}

TEST(Usd, ExportingAnEmptyMeshFailsCleanly) {
  const roadmaker::NetworkMesh empty;
  const auto result =
      roadmaker::export_usda(empty, std::filesystem::temp_directory_path() / "rm_empty.usda");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, roadmaker::ErrorCode::InvalidArgument);
}
