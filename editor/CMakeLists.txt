# Qt is provisioned locally by scripts/setup_qt.py (never FetchContent — too
# large) and discovered via cmake/QtVersion.cmake. LGPLv3, dynamic linking
# only, confined to editor targets.
find_package(Qt6 ${RM_QT_VERSION} QUIET COMPONENTS Widgets OpenGLWidgets)
if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Qt ${RM_QT_VERSION} not found. Run `python3 scripts/setup_qt.py` to "
    "provision it into ./.qt/, then re-configure.")
endif()

# Everything testable lives in the lib; the executable stays a thin shell so
# editor tests can link the same objects and run headless.
add_library(roadmaker_editor_lib STATIC
  src/document/diagnostic_locator.cpp
  src/render/scene_builder.cpp
  src/viewport/camera.cpp
  src/viewport/picking.cpp
)

target_include_directories(roadmaker_editor_lib PUBLIC src)

target_link_libraries(roadmaker_editor_lib
  PUBLIC roadmaker::core
  PRIVATE Eigen3::Eigen
)

rm_apply_warnings(roadmaker_editor_lib)

add_executable(roadmaker-editor
  src/app/main.cpp
  src/app/app.cpp
  src/render/gl_functions.cpp
  src/render/gl_renderer.cpp
)

target_link_libraries(roadmaker-editor PRIVATE
  roadmaker_editor_lib
  imgui::imgui
  glfw
)

rm_apply_warnings(roadmaker-editor)

if(RM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
