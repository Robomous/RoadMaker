cmake_minimum_required(VERSION 3.26)

project(roadmaker
  # Pre-release development version. The milestone/version model was retired
  # by ADR-0007 (Road to Parity reset): there will be exactly one release,
  # v0.1.0, so `main` carries 0.0.1 until the release gate passes.
  VERSION 0.0.1
  DESCRIPTION "Open source road-network authoring toolkit for AV simulation"
  HOMEPAGE_URL "https://github.com/robomous/roadmaker"
  LANGUAGES CXX C
)

option(RM_BUILD_TESTS "Build unit tests" ON)
option(RM_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(RM_BUILD_PYTHON "Build the Python bindings" OFF)
option(RM_BUILD_EDITOR "Build the Qt editor" OFF)
option(RM_BUILD_SHARED "Build roadmaker_core as a shared library" OFF)
# Stub until M2 phase 5 — the exporter lands per docs/design/m2/04_usd_export.md.
option(RM_BUILD_USD "Build the OpenUSD exporter" OFF)
option(RM_INSTALL "Generate install rules for the kernel" ${PROJECT_IS_TOP_LEVEL})
# The offline HTML manual (ADR-0009). OFF so a developer build never needs Node;
# the release packaging job builds it and passes ROADMAKER_MANUAL_DIR. No CMake
# target ever invokes npm.
option(ROADMAKER_BUNDLE_MANUAL "Install the prebuilt offline manual with the editor" OFF)
set(ROADMAKER_MANUAL_DIR "" CACHE PATH
  "Prebuilt manual directory (docs-site/dist) — required when ROADMAKER_BUNDLE_MANUAL is ON")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # nanobind module links the kernel

# Windows: keep <windows.h> from defining min/max macros anywhere in the build.
add_compile_definitions($<$<PLATFORM_ID:Windows>:NOMINMAX>)
# MSVC: sources and string literals are UTF-8 (fmt refuses to build without).
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>")

# Compiler cache. The caller decides: CI passes -DCMAKE_CXX_COMPILER_LAUNCHER
# explicitly (ccache on Linux/macOS, sccache on Windows) because the tool has
# to match what the workflow installed. Only when the caller says nothing do we
# fall back to auto-detecting ccache, which keeps developer builds behaving as
# they always have.
#
# This used to be an unconditional find_program(ccache), which silently gave
# MSVC no launcher at all — sccache is not named "ccache", so every Windows CI
# job was a full cold compile (19 min against Linux's 1.6 min). Never hardcode
# one cache tool's name here again.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_C_COMPILER_LAUNCHER)
  find_program(RM_CCACHE ccache)
  if(RM_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${RM_CCACHE})
    set(CMAKE_C_COMPILER_LAUNCHER ${RM_CCACHE})
  endif()
endif()

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/deps.cmake)

if(RM_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
endif()

add_subdirectory(core)

if(RM_BUILD_PYTHON AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/python/CMakeLists.txt)
  add_subdirectory(python)
endif()

if(RM_BUILD_EDITOR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt)
  include(cmake/QtVersion.cmake)
  add_subdirectory(editor)
  if(RM_INSTALL)
    include(GNUInstallDirs)
    include(cmake/Packaging.cmake)
  endif()
endif()
