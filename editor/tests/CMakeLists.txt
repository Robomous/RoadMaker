# GoogleTest is the runner (project standard); Qt6::Test is linked ONLY for
# QSignalSpy and QAbstractItemModelTester. The custom main sets the offscreen
# platform so the suite runs headless; the ctest ENVIRONMENT covers the
# discovery invocation on bare CI runners too.
find_package(Qt6 ${RM_QT_VERSION} REQUIRED COMPONENTS Test)

# Soak driver (hardening sprint §4.3): one library, two consumers — the
# standalone runner CI soaks under ASan, and the fixed-seed smoke gtest in
# the normal suite.
add_library(roadmaker_soak_lib STATIC soak/soak_driver.cpp)
target_include_directories(roadmaker_soak_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(roadmaker_soak_lib
  PUBLIC roadmaker_editor_lib
  PRIVATE fmt::fmt
)
rm_apply_warnings(roadmaker_soak_lib)

add_executable(roadmaker_soak soak/soak_main.cpp)
target_link_libraries(roadmaker_soak PRIVATE roadmaker_soak_lib)
rm_apply_warnings(roadmaker_soak)

if(WIN32 AND RM_BUILD_SHARED)
  # Same no-rpath story as the test executable below: the shared kernel DLL
  # must sit next to the runner.
  add_custom_command(TARGET roadmaker_soak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:roadmaker::core>
            $<TARGET_FILE_DIR:roadmaker_soak>)
endif()

add_executable(roadmaker_editor_tests
  qt_gtest_main.cpp
  test_autosave.cpp
  test_camera.cpp
  test_crash_handler.cpp
  test_create_junction_tool.cpp
  test_create_road_tool.cpp
  test_delete_tool.cpp
  test_diagnostics_model.cpp
  test_document.cpp
  test_document_commands.cpp
  test_edit_nodes_tool.cpp
  test_elevation_tool.cpp
  test_editor_command.cpp
  test_icons.cpp
  test_lane_profile.cpp
  test_panels.cpp
  test_picking.cpp
  test_preview_session.cpp
  test_select_tool.cpp
  test_scene_builder.cpp
  test_scene_tree_model.cpp
  test_selection_model.cpp
  test_soak_smoke.cpp
  test_tools.cpp
)

target_compile_definitions(roadmaker_editor_tests
  PRIVATE RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples")

target_link_libraries(roadmaker_editor_tests PRIVATE
  roadmaker_editor_lib
  roadmaker_soak_lib
  roadmaker::core
  Qt6::Test
  GTest::gtest
)

rm_apply_warnings(roadmaker_editor_tests)

if(WIN32 AND RM_BUILD_SHARED)
  # gtest_discover_tests runs the exe at build time and Windows has no rpath:
  # the shared kernel DLL must sit next to the test executable. Qt DLLs must
  # NOT be copied here — Qt's prefix is relocatable (resolved relative to
  # Qt6Core.dll), so a copied Qt6Core would look for the platform plugins in
  # the build tree and abort; Qt loads from the provisioned tree via PATH.
  add_custom_command(TARGET roadmaker_editor_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:roadmaker::core>
            $<TARGET_FILE_DIR:roadmaker_editor_tests>)
endif()

gtest_discover_tests(roadmaker_editor_tests
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
