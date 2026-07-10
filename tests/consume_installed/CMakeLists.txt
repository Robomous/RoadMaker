# Smoke test for the INSTALLED kernel package: configured by CI against a
# `cmake --install` prefix to prove find_package(roadmaker) + dynamic linking
# resolve outside the build tree. Not part of the superbuild.
cmake_minimum_required(VERSION 3.26)
project(roadmaker_consume_smoke LANGUAGES CXX)

# The installed package requires the consumer to provide tl-expected (its one
# public dependency). This sample supplies the same pinned copy and lets
# OVERRIDE_FIND_PACKAGE satisfy the config file's find_dependency(tl-expected).
include(FetchContent)
FetchContent_Declare(tl-expected
  URL https://github.com/TartanLlama/expected/archive/refs/tags/v1.3.1.tar.gz
  URL_HASH SHA256=9a04f4f472fbb5c30bf60402f1ca626c4a76987f867978d0b8a35d7ab3fb8fe7
  OVERRIDE_FIND_PACKAGE
)
set(EXPECTED_BUILD_TESTS OFF)

find_package(roadmaker CONFIG REQUIRED)

add_executable(consume_smoke main.cpp)
target_link_libraries(consume_smoke PRIVATE roadmaker::core)
target_compile_features(consume_smoke PRIVATE cxx_std_20)
