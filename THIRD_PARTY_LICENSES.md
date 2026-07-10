# Third-party licenses

Every dependency of RoadMaker, its exact pinned version, license, and what it
is used for. A dependency lands in `cmake/deps.cmake` and in this table in the
same commit. Allowed licenses: MIT, BSD-2/3, Apache-2.0, MPL-2.0, zlib,
BSL-1.0, Unlicense/CC0. License files verified in each upstream archive.

| Name | Version | License | URL | Usage |
|---|---|---|---|---|
| Eigen | 3.4.1 | MPL-2.0 | https://gitlab.com/libeigen/eigen | Linear algebra (header-only) |
| Clothoids | 2.1.0 | BSD-2-Clause | https://github.com/ebertolazzi/Clothoids | Clothoid/Euler-spiral math: G1/G2 fitting, arc-length evaluation |
| UtilsLite | 29d83dc (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/UtilsLite | Clothoids internal utilities (embeds fmt 11 © fmt contributors, MIT, in inline namespace fmt::v11) |
| quarticRootsFlocke | ad9028e (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/quarticRootsFlocke | Polynomial root solving used by Clothoids |
| GenericContainer | f98cd3d (Clothoids 2.1.0 submodule pin) | BSD-2-Clause | https://github.com/ebertolazzi/GenericContainer | Container type used by Clothoids API |
| Manifold | 3.5.2 | Apache-2.0 | https://github.com/elalish/manifold | Robust mesh booleans, junction solids |
| libigl | 2.6.0 | MPL-2.0 | https://github.com/libigl/libigl | Mesh processing (header-only; GPL `copyleft/` headers are never used) |
| CDT | 1.4.4 | MPL-2.0 | https://github.com/artem-ogre/CDT | 2D constrained Delaunay triangulation (junction floors) |
| Clipper2 | 2.0.1 | BSL-1.0 | https://github.com/AngusJohnson/Clipper2 | 2D polygon offsets/unions for plan-view lane logic |
| pugixml | 1.16 | MIT | https://github.com/zeux/pugixml | XML parsing/writing for OpenDRIVE |
| tinygltf | 3.0.0 | MIT | https://github.com/syoyo/tinygltf | glTF 2.0 export (bundles nlohmann/json, MIT; stb, MIT/public-domain) |
| tl::expected | 1.3.1 | CC0-1.0 | https://github.com/TartanLlama/expected | rm::Expected error returns (std::expected is C++23; kernel is C++20) |
| fast_float | 8.2.10 | Apache-2.0 OR MIT OR BSL-1.0 | https://github.com/fastfloat/fast_float | Locale-independent number parsing in xodr IO |
| GoogleTest | 1.17.0 | BSD-3-Clause | https://github.com/google/googletest | C++ unit tests (test builds only) |
| nanobind | >=2.1 (pip build requirement) | BSD-3-Clause | https://github.com/wjakob/nanobind | Python bindings (build-time; resolved via pyproject.toml) |
| GLFW | 3.4 | zlib | https://github.com/glfw/glfw | Editor windowing/input (editor builds only) |
| Dear ImGui | 1.92.8-docking | MIT | https://github.com/ocornut/imgui | Editor UI (editor builds only) |
| {fmt} | 12.2.0 | MIT | https://github.com/fmtlib/fmt | Formatting (kernel-wide, no iostream) |
| spdlog | 1.17.0 | MIT | https://github.com/gabime/spdlog | Logging (built against external fmt) |
