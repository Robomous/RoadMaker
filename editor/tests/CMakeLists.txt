add_executable(roadmaker_editor_tests
  test_camera.cpp
  test_diagnostic_locator.cpp
  test_picking.cpp
  test_scene_builder.cpp
)

target_compile_definitions(roadmaker_editor_tests
  PRIVATE RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples")

target_link_libraries(roadmaker_editor_tests PRIVATE
  roadmaker_editor_lib
  roadmaker::core
  GTest::gtest_main
)

rm_apply_warnings(roadmaker_editor_tests)

gtest_discover_tests(roadmaker_editor_tests)
