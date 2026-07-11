# Third-party dependencies — every entry pinned to an exact release archive
# URL + SHA256. Policy: MIT/BSD/Apache-2.0/MPL-2.0/zlib/BSL-1.0 only; every
# row here has a matching row in THIRD_PARTY_LICENSES.md.
#
# Header-only libraries we consume without running the upstream CMake use
# SOURCE_SUBDIR pointing at a directory with no CMakeLists so that
# FetchContent_MakeAvailable only populates sources; we then define our own
# INTERFACE/static targets below.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# Some upstream archives still declare cmake_minimum_required(<3.5), which
# CMake 4.x rejects outright without this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ---------------------------------------------------------------------------
# fmt 12.2.0 (MIT)
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz
  URL_HASH SHA256=8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0
)
set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_DOC OFF)

# ---------------------------------------------------------------------------
# spdlog 1.17.0 (MIT) — compiled against our external fmt, not its bundled one
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
)
set(SPDLOG_FMT_EXTERNAL ON)
set(SPDLOG_BUILD_EXAMPLE OFF)
set(SPDLOG_BUILD_TESTS OFF)
set(SPDLOG_INSTALL OFF)

# ---------------------------------------------------------------------------
# Eigen 3.4.1 (MPL-2.0) — header-only; upstream CMake not used
FetchContent_Declare(eigen
  URL https://gitlab.com/libeigen/eigen/-/archive/3.4.1/eigen-3.4.1.tar.gz
  URL_HASH SHA256=b93c667d1b69265cdb4d9f30ec21f8facbbe8b307cf34c0b9942834c6d4fdbe2
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# pugixml 1.16 (MIT)
FetchContent_Declare(pugixml
  URL https://github.com/zeux/pugixml/archive/refs/tags/v1.16.tar.gz
  URL_HASH SHA256=357bcab8877dc9943f355d3a72daba1b053238ba955f50fa81586afb65090219
)

# ---------------------------------------------------------------------------
# Clipper2 2.0.1 (BSL-1.0) — CMake lives in CPP/
FetchContent_Declare(clipper2
  URL https://github.com/AngusJohnson/Clipper2/archive/refs/tags/Clipper2_2.0.1.tar.gz
  URL_HASH SHA256=2a3693aceab4aed3e39b743e038d87701acc53cf05ed7b2013aab3e0aec5287e
  SOURCE_SUBDIR CPP
)
set(CLIPPER2_EXAMPLES OFF)
set(CLIPPER2_TESTS OFF)
set(CLIPPER2_UTILS OFF)
set(CLIPPER2_USINGZ OFF CACHE BOOL "" FORCE) # plan-view 2D only

# ---------------------------------------------------------------------------
# CDT 1.4.4 (MPL-2.0) — header-only; upstream CMake not used
FetchContent_Declare(cdt
  URL https://github.com/artem-ogre/CDT/archive/refs/tags/1.4.4.tar.gz
  URL_HASH SHA256=97e57bdd1cf8219dcc81634236a502390a20dda3599dd3414a74343b7f03427f
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# libigl 2.6.0 (MPL-2.0) — header-only mode ONLY; the GPL `copyleft/`
# headers must never be included by RoadMaker code (licensing policy).
FetchContent_Declare(libigl
  URL https://github.com/libigl/libigl/archive/refs/tags/v2.6.0.tar.gz
  URL_HASH SHA256=fe3bf58571cccbef774947261284ccf6b7fdf04fcab5f7181e31931e42a0b14f
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# Manifold 3.5.2 (Apache-2.0) — robust mesh booleans
FetchContent_Declare(manifold
  URL https://github.com/elalish/manifold/archive/refs/tags/v3.5.2.tar.gz
  URL_HASH SHA256=35cb5e0d78882f461ec39b17d8f09c2aceca761356f3ce948e3f3908289b8f2e
)
set(MANIFOLD_TEST OFF)
set(MANIFOLD_CROSS_SECTION OFF) # we use Clipper2 directly for plan-view ops
set(MANIFOLD_PAR OFF)
set(MANIFOLD_CBIND OFF)
set(MANIFOLD_PYBIND OFF)
set(MANIFOLD_JSBIND OFF)
set(MANIFOLD_DOWNLOADS OFF)

# ---------------------------------------------------------------------------
# tinygltf 3.0.0 (MIT; bundles nlohmann/json (MIT) and stb (MIT/public
# domain)) — header-only; implementation macro defined in core/src/io.
FetchContent_Declare(tinygltf
  URL https://github.com/syoyo/tinygltf/archive/refs/tags/v3.0.0.tar.gz
  URL_HASH SHA256=806b0f1ba8007837fcd531e23872286f8a8870ee23275e1eb5304cdb48e4cb30
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# Clothoids 2.1.0 (BSD-2) + its submodules, pinned at the exact commits the
# 2.1.0 tag references (GitHub release tarballs do not include submodules).
# We compile all four source drops into a single static library below
# instead of using upstream's Rake/cmake_utils build.
FetchContent_Declare(clothoids
  URL https://github.com/ebertolazzi/Clothoids/archive/refs/tags/2.1.0.tar.gz
  URL_HASH SHA256=2949703be2f02ef002ec00faeb45a81778b8804b6d898af3b66810e2e2695432
  SOURCE_SUBDIR cmake-disabled
)
# UtilsLite @ 29d83dc (submodule pin of Clothoids 2.1.0) — BSD-2
FetchContent_Declare(utilslite
  URL https://github.com/ebertolazzi/UtilsLite/archive/29d83dc87a6e461fa0c96f18d90f577da88f53fc.tar.gz
  URL_HASH SHA256=0f382f18425f2abe732cf5ff9b43f4bb0ce34afb6910fd10e652834865c18060
  SOURCE_SUBDIR cmake-disabled
)
# quarticRootsFlocke @ ad9028e (submodule pin) — BSD-2
FetchContent_Declare(quartic
  URL https://github.com/ebertolazzi/quarticRootsFlocke/archive/ad9028e4e6444cd496c83501b07eb8904c5a9c9b.tar.gz
  URL_HASH SHA256=d00690d9f7ded117cf517bed8001bcfc89d6e0587fa88f5449bfe6eb69d03505
  SOURCE_SUBDIR cmake-disabled
)
# GenericContainer @ f98cd3d (submodule pin) — BSD-2
FetchContent_Declare(gencon
  URL https://github.com/ebertolazzi/GenericContainer/archive/f98cd3d3bdf03cafebb91e37aec4a4874e00cc58.tar.gz
  URL_HASH SHA256=8279d999cbc121302f9dfc15ae0df4a6134ff0723e48418f037268e8ea2b471b
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# tl::expected 1.3.1 (CC0-1.0) — std::expected is C++23; we target C++20
FetchContent_Declare(tlexpected
  URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.3.1.tar.gz
  URL_HASH SHA256=9a04f4f472fbb5c30bf60402f1ca626c4a76987f867978d0b8a35d7ab3fb8fe7
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# fast_float 8.2.10 (Apache-2.0 OR MIT OR BSL-1.0) — locale-independent
# number parsing for xodr IO (std::from_chars for double is not yet
# universal across our CI stdlibs)
FetchContent_Declare(fastfloat
  URL https://github.com/fastfloat/fast_float/archive/refs/tags/v8.2.10.tar.gz
  URL_HASH SHA256=76f958dd97b1cf4d8862d1f0986a47d4bdfa8845252bae15ef0f40de3b95961f
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# tinyusdz v0.9.1 (Apache-2.0) — OpenUSD ASCII (.usda) export backend, gated on
# RM_BUILD_USD. Vendored permissive-licensed code: mapbox/eternal (ISC),
# linalg.h (Unlicense), jsteemann/atoi (Apache-2.0) — see THIRD_PARTY_LICENSES.
# Unlike the header-only deps above we DO use its own CMake (a hand-rolled
# source list would have to track 60+ TUs + vendored externals + the exact
# define set across three toolchains); we just trim it to the USDA writer and
# force C++17 to match the configuration validated in the spike.
if(RM_BUILD_USD)
  FetchContent_Declare(tinyusdz
    URL https://github.com/lighttransport/tinyusdz/archive/refs/tags/v0.9.1.tar.gz
    URL_HASH SHA256=7e3d6dd8f54bfa8c7afe830d4505f7740bc26d5055f5f2a603ee9585872933e2
  )
  # Trim upstream to the static core: no examples/tests/benchmarks, no image or
  # audio codecs, no MaterialX/FBX/Python/C-API. NO_WERROR is required
  # (upstream is not warning-clean on our toolchains).
  set(TINYUSDZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_USDMTLX OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_PYTHON OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_C_API OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_EXR OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_TIFF OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_USDFBX OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_AUDIO OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_USE_CCACHE OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_NO_WERROR ON CACHE BOOL "" FORCE)
endif()

# ---------------------------------------------------------------------------
# NOTE: the editor's UI toolkit (Qt 6, LGPLv3, dynamic linking only) is NOT a
# FetchContent dependency — it is provisioned by scripts/setup_qt.py and
# discovered via cmake/QtVersion.cmake. See THIRD_PARTY_LICENSES.md.

# ---------------------------------------------------------------------------
# GoogleTest 1.17.0 (BSD-3-Clause) — tests only (project testing standard)
if(RM_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  )
  set(BUILD_GMOCK OFF)
  set(INSTALL_GTEST OFF)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # MSVC runtime match
endif()

# ---------------------------------------------------------------------------
FetchContent_MakeAvailable(
  fmt spdlog eigen pugixml clipper2 cdt libigl manifold tinygltf
  clothoids utilslite quartic gencon tlexpected fastfloat)
if(RM_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
endif()
if(RM_BUILD_USD)
  FetchContent_MakeAvailable(tinyusdz)
  # Upstream builds the static lib as `tinyusdz_static` (alias
  # tinyusdz::tinyusdz_static) but attaches no INTERFACE include dirs — add our
  # own namespaced alias and a SYSTEM include path (src/) so our exporter can
  # #include "usda-writer.hh" without inheriting upstream header warnings under
  # -Werror. Pin it to C++17: as a subdirectory it does not force its own
  # standard, and it is not C++20-clean (same treatment as Clothoids).
  add_library(tinyusdz::tinyusdz ALIAS tinyusdz_static)
  target_include_directories(tinyusdz_static SYSTEM INTERFACE
    ${tinyusdz_SOURCE_DIR}/src)
  set_target_properties(tinyusdz_static PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
endif()

# In-tree upstream targets lack the namespaced aliases their installed
# packages export — add them so RoadMaker links one consistent spelling.
if(NOT TARGET Clipper2::Clipper2)
  add_library(Clipper2::Clipper2 ALIAS Clipper2)
endif()
if(NOT TARGET manifold::manifold)
  add_library(manifold::manifold ALIAS manifold)
endif()

# Their headers must not surface warnings in RoadMaker TUs (CI uses -Werror).
# NOTE: fmt/spdlog deliberately stay non-SYSTEM — Apple clang searches
# /usr/local/include before user -isystem dirs, so SYSTEM-ifying them lets a
# machine-installed spdlog/fmt shadow our pinned copies (ODR hazard).
set(_rm_system_include_deps Clipper2 manifold)
if(TARGET gtest)
  list(APPEND _rm_system_include_deps gtest)
endif()
foreach(_dep ${_rm_system_include_deps})
  get_target_property(_dep_inc ${_dep} INTERFACE_INCLUDE_DIRECTORIES)
  if(_dep_inc)
    set_target_properties(${_dep} PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dep_inc}")
  endif()
endforeach()

# ===========================================================================
# Hand-rolled targets for the header-only / no-CMake dependencies
# ===========================================================================

# Eigen
add_library(rm_eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS rm_eigen)
target_include_directories(rm_eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# CDT (header-only mode)
add_library(rm_cdt INTERFACE)
add_library(CDT::CDT ALIAS rm_cdt)
target_include_directories(rm_cdt SYSTEM INTERFACE ${cdt_SOURCE_DIR}/CDT/include)

# libigl (header-only, MPL-2.0 subset only)
add_library(rm_igl INTERFACE)
add_library(igl::core ALIAS rm_igl)
target_include_directories(rm_igl SYSTEM INTERFACE ${libigl_SOURCE_DIR}/include)
target_link_libraries(rm_igl INTERFACE Eigen3::Eigen)

# tinygltf (header-only; TINYGLTF_IMPLEMENTATION lives in core/src/io)
add_library(rm_tinygltf INTERFACE)
add_library(tinygltf::tinygltf ALIAS rm_tinygltf)
target_include_directories(rm_tinygltf SYSTEM INTERFACE ${tinygltf_SOURCE_DIR})
# These must be identical in every TU that includes tiny_gltf.h, or the
# implementation TU and consumers disagree about which symbols exist.
target_compile_definitions(rm_tinygltf INTERFACE
  TINYGLTF_NO_STB_IMAGE
  TINYGLTF_NO_STB_IMAGE_WRITE
  TINYGLTF_NO_EXTERNAL_IMAGE
)

# tl::expected (header-only)
add_library(rm_tlexpected INTERFACE)
add_library(tl::expected ALIAS rm_tlexpected)
target_include_directories(rm_tlexpected SYSTEM INTERFACE ${tlexpected_SOURCE_DIR}/include)

# fast_float (header-only)
add_library(rm_fastfloat INTERFACE)
add_library(FastFloat::fast_float ALIAS rm_fastfloat)
target_include_directories(rm_fastfloat SYSTEM INTERFACE ${fastfloat_SOURCE_DIR}/include)

# Clothoids + UtilsLite + quarticRootsFlocke + GenericContainer as one
# static library. UtilsLite embeds fmt 11 in an inline fmt::v11 namespace —
# distinct symbols from our fmt 12, but never include Clothoids/Utils
# headers and fmt/spdlog in the same public header.
file(GLOB rm_clothoids_sources
  ${clothoids_SOURCE_DIR}/src/*.cc
  ${utilslite_SOURCE_DIR}/src/*.cc
  ${quartic_SOURCE_DIR}/src/*.cc
  ${gencon_SOURCE_DIR}/src/*.cc
)
add_library(rm_clothoids STATIC ${rm_clothoids_sources})
add_library(Clothoids::Clothoids ALIAS rm_clothoids)
target_include_directories(rm_clothoids SYSTEM PUBLIC
  ${clothoids_SOURCE_DIR}/src
  ${utilslite_SOURCE_DIR}/src
  ${quartic_SOURCE_DIR}/src
  ${gencon_SOURCE_DIR}/include
)
# Upstream builds these sources as C++17; under C++20 they trip fmt's
# consteval format-string checks and the deleted char8_t ostream overloads.
set_target_properties(rm_clothoids PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
find_package(Threads REQUIRED)
target_link_libraries(rm_clothoids PUBLIC Threads::Threads)
# Third-party code is built quietly; our warnings apply to roadmaker targets only.
# Force-include <algorithm>: GenericContainer uses std::copy_n without
# including it (compiles on libc++ only by transitive luck).
if(MSVC)
  target_compile_options(rm_clothoids PRIVATE /FIalgorithm)
else()
  target_compile_options(rm_clothoids PRIVATE -w -include algorithm)
endif()
