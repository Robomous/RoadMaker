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
  test_crosswalk_item.cpp
  test_crosswalk_placement.cpp
  test_crosswalk_stop_line_tool.cpp
  test_marking_curve_placement.cpp
  test_marking_curve_tool.cpp
  test_marking_point_tool.cpp
  test_prop_placement.cpp
  test_prop_point_tool.cpp
  test_prop_curve_tool.cpp
  test_prop_span_tool.cpp
  test_corner_tool.cpp
  test_junction_span_tool.cpp
  test_junction_surface_tool.cpp
  test_maneuver_tool.cpp
  test_stopline_tool.cpp
  test_prop_polygon_tool.cpp
  test_stencil_item.cpp
  test_stencil_placement.cpp
  test_delete_tool.cpp
  test_diagnostics_model.cpp
  test_document.cpp
  test_editor2d_host.cpp
  test_document_commands.cpp
  test_edit_nodes_tool.cpp
  test_junction_regen.cpp
  test_elevation_tool.cpp
  test_lane_add_tool.cpp
  test_lane_carve_tool.cpp
  test_lane_form_tool.cpp
  test_editor_command.cpp
  test_highlight.cpp
  test_icons.cpp
  test_library_drop.cpp
  test_library_model.cpp
  test_library_panel.cpp
  test_lane_profile.cpp
  test_log_setup.cpp
  test_main_window_layout.cpp
  test_mesh_parity.cpp
  test_nav_controller.cpp
  test_panels.cpp
  test_framing.cpp
  test_gizmo.cpp
  test_help_collection.cpp
  test_help_compiler.cpp
  test_help_registry.cpp
  test_help_style.cpp
  test_help_viewer.cpp
  test_picking.cpp
  test_preview_session.cpp
  test_profile_panel.cpp
  test_project.cpp
  test_projection.cpp
  test_context_menu.cpp
  test_select_tool.cpp
  test_split_tool.cpp
  test_scene_builder.cpp
  test_material_catalog.cpp
  test_scrub_label.cpp
  test_slot_widget.cpp
  test_scene_tree_model.cpp
  test_selection_model.cpp
  test_shortcut_registry.cpp
  test_toolbar_registry.cpp
  test_soak_smoke.cpp
  test_theme.cpp
  test_toast_queue.cpp
  test_tour_controller.cpp
  test_tools.cpp
  test_welcome_widget.cpp
  test_width_panel.cpp
)

target_compile_definitions(roadmaker_editor_tests
  PRIVATE RM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/assets/samples"
          RM_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
          # The shortcut-map gate reads the committed page and compares it to
          # what the registry renders (test_shortcut_registry.cpp).
          RM_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs"
          # The help-style gate compares the committed help.css to the
          # generator (test_help_style.cpp).
          RM_EDITOR_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/editor/resources"
          # Where the build stages the compiled .qch/.qhc collection
          # (test_help_collection.cpp / test_help_viewer.cpp).
          RM_HELP_STAGE_DIR="${CMAKE_BINARY_DIR}/help")

target_link_libraries(roadmaker_editor_tests PRIVATE
  roadmaker_editor_lib
  roadmaker_soak_lib
  roadmaker::core
  helpc_lib
  Qt6::Test
  Qt6::Help
  GTest::gtest
)

# The collection/viewer tests read the staged .qch/.qhc, so it must be built
# first (the help target is ALL, but discovery runs the exe at build time).
add_dependencies(roadmaker_editor_tests roadmaker_help)

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
