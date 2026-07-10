// Build-system smoke test: proves every pinned dependency compiles, links,
// and does something trivially correct on this toolchain.

#include "roadmaker/version.hpp"

#include <CDT.h>
#include <Clothoids.hh>
#include <Eigen/Dense>
#include <clipper2/clipper.h>
#include <manifold/manifold.h>
#include <pugixml.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <numbers>
#include <string>

TEST(Smoke, KernelReportsItsVersion) {
  // The string comes from PROJECT_VERSION; assert the semver shape rather
  // than a literal so version bumps cannot silently break the suite.
  const std::string version(roadmaker::version());
  EXPECT_EQ(std::count(version.begin(), version.end(), '.'), 2);
  EXPECT_TRUE(std::all_of(version.begin(), version.end(), [](unsigned char c) {
    return (std::isdigit(c) != 0) || c == '.';
  }));
}

TEST(Smoke, EigenSolvesALinearSystem) {
  Eigen::Matrix2d a;
  a << 2.0, 0.0, 0.0, 4.0;
  const Eigen::Vector2d x = a.ldlt().solve(Eigen::Vector2d{2.0, 8.0});
  EXPECT_NEAR(x.x(), 1.0, 1e-12);
  EXPECT_NEAR(x.y(), 2.0, 1e-12);
}

TEST(Smoke, ClothoidsBuildsAG1HermiteFit) {
  G2lib::ClothoidCurve curve{"smoke"};
  // Quarter-turn: from origin heading +X to (10, 10) heading +Y.
  const int ok = curve.build_G1(0.0, 0.0, 0.0, 10.0, 10.0, std::numbers::pi / 2.0);
  ASSERT_GT(ok, 0);
  EXPECT_GT(curve.length(), 14.0); // longer than the straight-line distance
  EXPECT_LT(curve.length(), 20.0);
}

TEST(Smoke, Clipper2UnionsTwoSquares) {
  Clipper2Lib::PathsD subject{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}};
  Clipper2Lib::PathsD clip{{{5, 5}, {15, 5}, {15, 15}, {5, 15}}};
  const auto solution = Clipper2Lib::Union(subject, clip, Clipper2Lib::FillRule::NonZero);
  EXPECT_EQ(solution.size(), 1U);
}

TEST(Smoke, CdtTriangulatesASquare) {
  CDT::Triangulation<double> cdt;
  cdt.insertVertices({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
  cdt.eraseSuperTriangle();
  EXPECT_EQ(cdt.triangles.size(), 2U);
}

TEST(Smoke, ManifoldBuildsAWatertightCube) {
  const manifold::Manifold cube = manifold::Manifold::Cube({1.0, 1.0, 1.0});
  EXPECT_NEAR(cube.Volume(), 1.0, 1e-3);
  EXPECT_EQ(cube.Status(), manifold::Manifold::Error::NoError);
}

TEST(Smoke, PugixmlParsesADocument) {
  pugi::xml_document doc;
  const pugi::xml_parse_result result =
      doc.load_string(R"(<OpenDRIVE><header revMajor="1" revMinor="7"/></OpenDRIVE>)");
  ASSERT_TRUE(result);
  EXPECT_EQ(doc.child("OpenDRIVE").child("header").attribute("revMinor").as_int(), 7);
}
