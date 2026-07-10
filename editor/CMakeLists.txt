# Qt is provisioned locally by scripts/setup_qt.py (never FetchContent — too
# large) and discovered via cmake/QtVersion.cmake. LGPLv3, dynamic linking
# only, confined to editor targets.
find_package(Qt6 ${RM_QT_VERSION} QUIET COMPONENTS Widgets OpenGLWidgets)
if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Qt ${RM_QT_VERSION} not found. Run `python3 scripts/setup_qt.py` to "
    "provision it into ./.qt/, then re-configure.")
endif()

# macOS: Qt 6.8's FindWrapOpenGL still links the AGL framework (either as a
# stale /System/Library path or a literal "-framework AGL" fallback) — but
# Apple removed AGL from current SDKs, so ld fails on every modern toolchain,
# local and CI. Strip every AGL entry; the editor uses core-profile GL via
# the OpenGL framework only. (No "AGL" substring exists in the other Qt link
# items, so the regex is exact enough.)
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  get_target_property(_rm_wrap_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
  if(_rm_wrap_gl_libs)
    list(FILTER _rm_wrap_gl_libs EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL
      PROPERTIES INTERFACE_LINK_LIBRARIES "${_rm_wrap_gl_libs}")
  endif()
endif()

# Everything testable lives in the lib; the executable stays a thin shell so
# editor tests can link the same objects and run headless (offscreen QPA).
add_library(roadmaker_editor_lib STATIC
  src/app/actions.cpp
  src/app/main_window.cpp
  src/app/settings.cpp
  src/document/diagnostic_locator.cpp
  src/document/diagnostics_model.cpp
  src/document/document.cpp
  src/document/scene_tree_model.cpp
  src/document/selection_model.cpp
  src/panels/diagnostics_panel.cpp
  src/panels/properties_panel.cpp
  src/panels/scene_tree_panel.cpp
  src/render/gl_functions.cpp
  src/render/gl_renderer.cpp
  src/render/scene_builder.cpp
  src/viewport/camera.cpp
  src/viewport/picking.cpp
  src/viewport/viewport_widget.cpp
)

set_target_properties(roadmaker_editor_lib PROPERTIES AUTOMOC ON)

target_include_directories(roadmaker_editor_lib PUBLIC src)

# spdlog is the editor's own logging dependency (not inherited through the
# kernel); Eigen backs the picking math only.
target_link_libraries(roadmaker_editor_lib
  PUBLIC
    roadmaker::core
    Qt6::Widgets
    Qt6::OpenGLWidgets
  PRIVATE
    Eigen3::Eigen
    spdlog::spdlog
)

rm_apply_warnings(roadmaker_editor_lib)

qt_add_executable(roadmaker-editor src/main.cpp)

target_link_libraries(roadmaker-editor PRIVATE roadmaker_editor_lib)

rm_apply_warnings(roadmaker-editor)

if(RM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
