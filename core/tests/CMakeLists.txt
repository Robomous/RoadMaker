add_executable(roadmaker_core_tests
  test_smoke.cpp
  test_id_arena.cpp
  test_network.cpp
  test_reference_line.cpp
  test_profile_fit.cpp
  test_authoring.cpp
  test_xodr_reader.cpp
  test_xodr_writer.cpp
  test_xodr_objects.cpp
  test_xodr_signals.cpp
  test_road_marks.cpp
  test_mesh.cpp
  test_junction_surface.cpp
  test_junction_export.cpp
  test_t_junction_quality.cpp
  test_gltf.cpp
  test_round_trip.cpp
  test_edit_stack.cpp
  test_edit_operations.cpp
  test_edit_snap.cpp
  test_remesh_budget.cpp
)

# The USD exporter and its golden tests only exist when RM_BUILD_USD=ON. The
# test reads back the emitted USDA text, so it needs no tinyusdz link.
if(RM_BUILD_USD)
  target_sources(roadmaker_core_tests PRIVATE test_usd.cpp)
endif()

target_compile_definitions(roadmaker_core_tests PRIVATE
  RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples"
  RM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/junctions"
  RM_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus"
)

if(RM_BUILD_SHARED)
  # See the comment in tinygltf_impl.cpp — shared builds hide core's copy.
  target_sources(roadmaker_core_tests PRIVATE tinygltf_impl.cpp)
endif()

# Third-party deps are re-listed because core links them PRIVATE — including
# Eigen/fmt/spdlog, which are no longer part of core's public interface.
target_link_libraries(roadmaker_core_tests
  PRIVATE
    roadmaker::core
    Eigen3::Eigen
    Clothoids::Clothoids
    CDT::CDT
    Clipper2::Clipper2
    manifold::manifold
    pugixml::pugixml
    tinygltf::tinygltf
    GTest::gtest_main
)

rm_apply_warnings(roadmaker_core_tests)

if(WIN32 AND RM_BUILD_SHARED)
  # gtest_discover_tests runs the exe at build time and Windows has no rpath:
  # the kernel DLL must sit next to the test executable.
  add_custom_command(TARGET roadmaker_core_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:roadmaker_core_tests>
            $<TARGET_FILE_DIR:roadmaker_core_tests>
    COMMAND_EXPAND_LISTS)
endif()

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
