add_executable(roadmaker_core_tests
  test_smoke.cpp
  test_id_arena.cpp
  test_network.cpp
  test_reference_line.cpp
  test_xodr_reader.cpp
  test_mesh.cpp
  test_gltf.cpp
  test_round_trip.cpp
)

target_compile_definitions(roadmaker_core_tests PRIVATE
  RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples"
)

target_link_libraries(roadmaker_core_tests
  PRIVATE
    roadmaker::core
    Clothoids::Clothoids
    CDT::CDT
    Clipper2::Clipper2
    manifold::manifold
    pugixml::pugixml
    tinygltf::tinygltf
    GTest::gtest_main
)

rm_apply_warnings(roadmaker_core_tests)

gtest_discover_tests(roadmaker_core_tests)

if(RM_BUILD_FUZZERS)
  # AppleClang from Command Line Tools does not ship the libFuzzer runtime
  # (libclang_rt.fuzzer_osx.a) — fuzzing runs in the Linux CI job.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_subdirectory(fuzz)
  else()
    message(WARNING "RM_BUILD_FUZZERS requires upstream Clang (libFuzzer); skipping")
  endif()
endif()
