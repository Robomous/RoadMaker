# GoogleTest is the runner (project standard); Qt6::Test is linked ONLY for
# QSignalSpy and QAbstractItemModelTester. The custom main sets the offscreen
# platform so the suite runs headless; the ctest ENVIRONMENT covers the
# discovery invocation on bare CI runners too.
find_package(Qt6 ${RM_QT_VERSION} REQUIRED COMPONENTS Test)

add_executable(roadmaker_editor_tests
  qt_gtest_main.cpp
  test_camera.cpp
  test_diagnostic_locator.cpp
  test_diagnostics_model.cpp
  test_document.cpp
  test_panels.cpp
  test_picking.cpp
  test_scene_builder.cpp
  test_scene_tree_model.cpp
  test_selection_model.cpp
)

target_compile_definitions(roadmaker_editor_tests
  PRIVATE RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples")

target_link_libraries(roadmaker_editor_tests PRIVATE
  roadmaker_editor_lib
  roadmaker::core
  Qt6::Test
  GTest::gtest
)

rm_apply_warnings(roadmaker_editor_tests)

gtest_discover_tests(roadmaker_editor_tests
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
