// Build-system smoke test: proves every pinned dependency compiles, links,
// and does something trivially correct on this toolchain.

#include "roadmaker/version.hpp"

#include <CDT.h>
#include <Clothoids.hh>
#include <Eigen/Dense>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <clipper2/clipper.h>
#include <manifold/manifold.h>
#include <pugixml.hpp>

#include <numbers>

TEST_CASE("kernel reports its version", "[smoke]") {
  REQUIRE(roadmaker::version() == "0.1.0");
}

TEST_CASE("Eigen solves a linear system", "[smoke][deps]") {
  Eigen::Matrix2d a;
  a << 2.0, 0.0, 0.0, 4.0;
  const Eigen::Vector2d x = a.ldlt().solve(Eigen::Vector2d{2.0, 8.0});
  REQUIRE_THAT(x.x(), Catch::Matchers::WithinAbs(1.0, 1e-12));
  REQUIRE_THAT(x.y(), Catch::Matchers::WithinAbs(2.0, 1e-12));
}

TEST_CASE("Clothoids builds a G1 Hermite fit", "[smoke][deps]") {
  G2lib::ClothoidCurve curve{"smoke"};
  // Quarter-turn: from origin heading +X to (10, 10) heading +Y.
  const int ok = curve.build_G1(0.0, 0.0, 0.0, 10.0, 10.0, std::numbers::pi / 2.0);
  REQUIRE(ok > 0);
  REQUIRE(curve.length() > 14.0); // longer than the straight-line distance
  REQUIRE(curve.length() < 20.0);
}

TEST_CASE("Clipper2 unions two squares", "[smoke][deps]") {
  Clipper2Lib::PathsD subject{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}};
  Clipper2Lib::PathsD clip{{{5, 5}, {15, 5}, {15, 15}, {5, 15}}};
  const auto solution = Clipper2Lib::Union(subject, clip, Clipper2Lib::FillRule::NonZero);
  REQUIRE(solution.size() == 1);
}

TEST_CASE("CDT triangulates a square", "[smoke][deps]") {
  CDT::Triangulation<double> cdt;
  cdt.insertVertices({{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
  cdt.eraseSuperTriangle();
  REQUIRE(cdt.triangles.size() == 2);
}

TEST_CASE("Manifold builds a watertight cube", "[smoke][deps]") {
  const manifold::Manifold cube = manifold::Manifold::Cube({1.0, 1.0, 1.0});
  REQUIRE(cube.Volume() > 0.999);
  REQUIRE(cube.Volume() < 1.001);
  REQUIRE(cube.Status() == manifold::Manifold::Error::NoError);
}

TEST_CASE("pugixml parses a document", "[smoke][deps]") {
  pugi::xml_document doc;
  const pugi::xml_parse_result result =
      doc.load_string(R"(<OpenDRIVE><header revMajor="1" revMinor="7"/></OpenDRIVE>)");
  REQUIRE(result);
  REQUIRE(doc.child("OpenDRIVE").child("header").attribute("revMinor").as_int() == 7);
}
