cmake_minimum_required(VERSION 3.26)

project(roadmaker
  VERSION 0.1.0
  DESCRIPTION "Open source road-network authoring toolkit for AV simulation"
  HOMEPAGE_URL "https://github.com/robomous/roadmaker"
  LANGUAGES CXX C
)

option(RM_BUILD_TESTS "Build unit tests" ON)
option(RM_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(RM_BUILD_PYTHON "Build the Python bindings" OFF)
option(RM_BUILD_EDITOR "Build the ImGui editor" OFF)

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

find_program(RM_CCACHE ccache)
if(RM_CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${RM_CCACHE})
  set(CMAKE_C_COMPILER_LAUNCHER ${RM_CCACHE})
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
  add_subdirectory(editor)
endif()
