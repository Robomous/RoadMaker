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
#
# ★ THE ONE DEPENDENCY FETCHED BY GIT RATHER THAN BY URL + URL_HASH, and the
# departure is deliberate.
#
# On 2026-07-31 every fresh configure started failing with a URL_HASH mismatch:
#   expected 806b0f1ba8007837fcd531e23872286f8a8870ee23275e1eb5304cdb48e4cb30
#   actual   456b89f51ec64e0e982bc5e3ab57e945092b3005a8e726b271d202850619c643
# The tag had NOT moved — v3.0.0 still points at cfcadfa8, dated 2026-03-23,
# the same commit it was released from. GitHub simply RE-ROLLED the
# auto-generated source archive: `/archive/refs/tags/*.tar.gz` is built on
# demand and is explicitly not guaranteed byte-stable, so its SHA-256 can change
# under a fixed tag with no upstream change at all.
#
# A URL_HASH over a generated archive therefore pins the WRAPPER, not the
# content, and breaks on a schedule nobody controls. GIT_TAG at a full commit
# SHA pins the TREE — a strictly stronger integrity guarantee than the hash it
# replaces, and immune to re-rolls. Hence the exception to this file's
# URL + URL_HASH convention; do not "restore consistency" by reverting it.
#
# Use the full 40-character SHA, never the tag name: a tag is a movable ref and
# would reintroduce exactly the mutability this is here to remove.
FetchContent_Declare(tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG cfcadfa8d14eb489d97b6324838ae100410edcc7 # v3.0.0
  GIT_SHALLOW FALSE # a shallow clone cannot resolve an arbitrary commit SHA
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# stb (MIT OR public-domain) — stb_truetype.h only, for CPU text rasterisation
# of editable sign faces (roadmaker::signs::render_face). Header-only; the
# STB_TRUETYPE_IMPLEMENTATION macro is defined in exactly one TU
# (core/src/assets/sign_face.cpp). No tagged releases upstream, so pin the
# exact commit archive + SHA256 (disclosed in THIRD_PARTY_LICENSES.md).
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
  URL_HASH SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# nanosvg (zlib) — nanosvg.h + nanosvgrast.h, for CPU rasterisation of the
# baked US sign-pack symbol artwork (roadmaker::signs::render_face). The
# symbols are authored as SVG and embedded as SVG *text*, so the kernel
# rasterises them at the size each face needs — Qt's SVG renderer is
# editor-only and core must never link Qt, and the glTF exporter bakes faces
# headless and from Python. Header-only; NANOSVG_IMPLEMENTATION and
# NANOSVGRAST_IMPLEMENTATION are defined in exactly one TU
# (core/src/assets/sign_face.cpp). No tagged releases upstream, so pin the
# exact commit archive + SHA256 (disclosed in THIRD_PARTY_LICENSES.md).
FetchContent_Declare(nanosvg
  URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
  URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# laz-perf 3.4.0 (Apache-2.0) — the LAZ arithmetic decoder for the lidar
# importer (roadmaker::lidar). ADR-0011: PDAL was the roadmap's named vehicle
# and is not taken, because its cmake reads `find_package(PROJ 9.0 REQUIRED)`
# and `find_package(GDAL CONFIG REQUIRED)` — the dependency ADR-0010 declined.
# laz-perf is the opposite proposition: its own root CMakeLists calls
# find_package ZERO times. LAS itself is read by RoadMaker; this decodes the
# compressed point stream and nothing else.
#
# BUILT FROM SOURCE, NOT VIA ITS OWN CMakeLists, which is why SOURCE_SUBDIR
# disables it (the same idiom the twelve declarations above use). Upstream's
# cpp/CMakeLists.txt would give us three things we do not want and cannot switch
# off: a SHARED `lazperf` target declared explicitly rather than through
# BUILD_SHARED_LIBS, so forcing static does not suppress it; UNGUARDED
# `add_subdirectory(benchmarks)` and `add_subdirectory(tools)`, which build
# executables nothing here runs; and UNGUARDED `install(FILES ...)` rules that
# would put lazperf headers into RoadMaker's own install tree. Its
# `WITH_TESTS` default is worse than any of them — the ON path runs
# `file(DOWNLOAD ...)` of a sample tile AT CONFIGURE TIME, so a build that must
# be reproducible offline would reach the network to configure.
#
# The library is a flat source list, so compiling it directly is both smaller
# than fighting those defaults and exactly as reproducible.
#
# No release ASSET is published upstream (checked 2026-07-28: the 3.4.0 release
# carries none), so this is a forge-generated tag archive — the shape libtiff
# below deliberately avoids. Accepted here because there is no alternative
# artifact, and it is the same footing as the stb and nanosvg pins above. If a
# clean build ever fails its URL_HASH, that is why.
FetchContent_Declare(lazperf
  URL https://github.com/hobuinc/laz-perf/archive/refs/tags/3.4.0.tar.gz
  URL_HASH SHA256=ddc1219cac345aee53a33b52dde6b28892e85708b848ab6831dc0c9aa795534d
  SOURCE_SUBDIR cmake-disabled
)

# ---------------------------------------------------------------------------
# libtiff 4.7.2 (BSD-style "libtiff" licence; maintainer-approved per-case,
# ADR-0010 — the SPDX id `libtiff` is not literally on the allowed list, and
# the LZW code carries an ADDITIONAL UC Berkeley acknowledgement obligation
# discharged in THIRD_PARTY_LICENSES.md). GeoTIFF pixel decoding for the GIS
# importer (roadmaker::gis).
#
# Pinned to the OSGeo release tarball, NOT a forge-generated archive: GitLab
# and GitHub build those on demand and their bytes have historically changed
# across server versions, which would break URL_HASH for everyone on a later
# clean build. This one is a published, immutable artifact.
#
# Every codec that would need a dependency we do not have is OFF — that is why
# ADR-0010 lists Deflate and JPEG-in-TIFF as refused-by-name (#484) rather than
# quietly failing at read time. The internal-only codecs we actually use (LZW,
# PackBits) stay on; the rest are off per the policy's "minimum options" rule.
FetchContent_Declare(tiff
  URL https://download.osgeo.org/libtiff/tiff-4.7.2.tar.gz
  URL_HASH SHA256=672bd7d10aee4606171afb864f3570b83340f6a33e2c186dc0512f7145ffdf6a
)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-deprecated OFF CACHE BOOL "" FORCE)
set(tiff-install OFF CACHE BOOL "" FORCE)
set(tiff-cxx OFF CACHE BOOL "" FORCE)      # we use the C API only
set(tiff-opengl OFF CACHE BOOL "" FORCE)   # tiffgt viewer; would drag in GL
set(sphinx OFF CACHE BOOL "" FORCE)
# APPLE defaults this ON, which would build libtiff as a macOS Framework and
# make the static link we want impossible. Platform-conditional by upstream,
# so it must be forced unconditionally here.
set(tiff-framework OFF CACHE BOOL "" FORCE)
set(ld-version-script OFF CACHE BOOL "" FORCE)
# Codecs requiring an external library (#484 tracks zlib + libjpeg).
set(zlib OFF CACHE BOOL "" FORCE)
set(libdeflate OFF CACHE BOOL "" FORCE)
set(pixarlog OFF CACHE BOOL "" FORCE)      # requires zlib
set(jpeg OFF CACHE BOOL "" FORCE)
set(old-jpeg OFF CACHE BOOL "" FORCE)
set(jpeg12 OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
# Internal codecs: keep only what a GeoTIFF we accept can be compressed with.
set(lzw ON CACHE BOOL "" FORCE)
set(packbits ON CACHE BOOL "" FORCE)
set(ccitt OFF CACHE BOOL "" FORCE)         # bilevel fax; not imagery or DEM
set(thunder OFF CACHE BOOL "" FORCE)
set(next OFF CACHE BOOL "" FORCE)
set(logluv OFF CACHE BOOL "" FORCE)
set(mdi OFF CACHE BOOL "" FORCE)

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
# md4c 0.5.2 (MIT) — Markdown→HTML for the editor's build-time help compiler
# (rm_helpc). C99, tiny, no dependencies. Editor-only, and never linked into
# the shipped editor — it lives in the host tool that produces the .qch.
if(RM_BUILD_EDITOR)
  FetchContent_Declare(md4c
    URL https://github.com/mity/md4c/archive/refs/tags/release-0.5.2.tar.gz
    URL_HASH SHA256=55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21
  )
  set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE) # library only, no CLI
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
  fmt spdlog eigen pugixml clipper2 cdt manifold tinygltf stb nanosvg
  clothoids utilslite quartic gencon tlexpected fastfloat lazperf)
# libtiff defaults BUILD_SHARED_LIBS ON; force the static build so the kernel
# stays a single artifact with nothing to deploy beside it (same save/force/
# restore dance md4c needs below, for the same reason).
set(_rm_saved_build_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(tiff)
set(BUILD_SHARED_LIBS "${_rm_saved_build_shared}")
# Upstream already provides the TIFF::tiff alias. What it does NOT attach is a
# usable INTERFACE include path for an in-tree build: tiffio.h lives in the
# source tree while the generated tif_config.h/tiffconf.h land in the BUILD
# tree, so both are needed. BUILD_INTERFACE keeps these absolute paths out of
# any exported target (roadmaker installs a CMake package when
# RM_BUILD_SHARED=ON, and install(EXPORT) rejects bare build-tree paths).
target_include_directories(tiff SYSTEM INTERFACE
  $<BUILD_INTERFACE:${tiff_SOURCE_DIR}/libtiff>
  $<BUILD_INTERFACE:${tiff_BINARY_DIR}/libtiff>)
if(RM_BUILD_TESTS)
  FetchContent_MakeAvailable(googletest)
endif()
if(RM_BUILD_EDITOR)
  # Force a static md4c (upstream defaults to a shared lib on non-Windows) so
  # the help compiler is a self-contained host tool with nothing to deploy.
  set(_rm_saved_build_shared "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(md4c)
  set(BUILD_SHARED_LIBS "${_rm_saved_build_shared}")
  # As an in-tree subdirectory, md4c-html exposes no INTERFACE include dir. Wrap
  # it in BUILD_INTERFACE: md4c carries install(EXPORT) rules that reject a bare
  # build/source-tree path on an exported target.
  target_include_directories(md4c-html INTERFACE $<BUILD_INTERFACE:${md4c_SOURCE_DIR}/src>)
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

# tinygltf (header-only; TINYGLTF_IMPLEMENTATION lives in core/src/io — and ONLY
# there: core/src/assets/prop_import.cpp reads glTF through the same copy,
# including the header without the macro)
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

# stb (header-only; STB_TRUETYPE_IMPLEMENTATION lives in core/src/assets,
# STB_IMAGE_IMPLEMENTATION in core/src/gis — one TU each, never both here.
# core/src/assets/prop_import.cpp is a SECOND consumer of stb_image: it includes
# the header WITHOUT the implementation macro and links against the copy in
# core/src/gis/world_file.cpp, mirroring that TU's STBI_NO_* set so the two agree
# about which symbols exist.)
add_library(rm_stb INTERFACE)
add_library(stb::stb ALIAS rm_stb)
target_include_directories(rm_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# nlohmann/json — NOT a new download. It is the copy tinygltf already vendors
# (declared in tinygltf's THIRD_PARTY_LICENSES.md row), exposed under its own
# target so the GeoJSON reader can say what it depends on. Consumers include
# <json.hpp>, tinygltf's own spelling of it.
add_library(rm_json INTERFACE)
add_library(nlohmann::json ALIAS rm_json)
target_include_directories(rm_json SYSTEM INTERFACE ${tinygltf_SOURCE_DIR})

# laz-perf — compiled here rather than by its own CMakeLists (see the pin above
# for the three unguarded things that build would add). The library is a flat
# source list, so this is the whole of it.
#
# ★ LAZPERF_VENDORED IS UPSTREAM'S OWN MACRO FOR EXACTLY THIS CASE, and it is
# not optional. Without it `LAZPERF_EXPORT` expands to __declspec(dllexport) on
# Windows — inside a STATIC library, which is wrong and which MSVC diagnoses —
# and to __attribute__((visibility("default"))) elsewhere, publishing symbols
# from a dependency the kernel does not re-export. With it, the macro is empty.
file(GLOB _rm_lazperf_sources
  ${lazperf_SOURCE_DIR}/cpp/lazperf/*.cpp
  ${lazperf_SOURCE_DIR}/cpp/lazperf/detail/*.cpp)
add_library(rm_lazperf STATIC ${_rm_lazperf_sources})
add_library(lazperf::lazperf ALIAS rm_lazperf)
target_compile_definitions(rm_lazperf PUBLIC LAZPERF_VENDORED)
# SYSTEM so the dependency's own warnings never fail our -Werror build, and
# BUILD_INTERFACE so the absolute path stays out of any exported target
# (roadmaker installs a CMake package when RM_BUILD_SHARED=ON, and
# install(EXPORT) rejects bare build-tree paths).
target_include_directories(rm_lazperf SYSTEM PUBLIC
  $<BUILD_INTERFACE:${lazperf_SOURCE_DIR}/cpp>)
set_target_properties(rm_lazperf PROPERTIES
  POSITION_INDEPENDENT_CODE ON  # RM_BUILD_SHARED links it into a shared kernel
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)

# nanosvg (header-only; NANOSVG*_IMPLEMENTATION lives in core/src/assets)
add_library(rm_nanosvg INTERFACE)
add_library(nanosvg::nanosvg ALIAS rm_nanosvg)
target_include_directories(rm_nanosvg SYSTEM INTERFACE ${nanosvg_SOURCE_DIR}/src)

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
