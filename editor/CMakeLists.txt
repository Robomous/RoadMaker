# Qt is provisioned locally by scripts/setup_qt.py (never FetchContent — too
# large) and discovered via cmake/QtVersion.cmake. LGPLv3, dynamic linking
# only, confined to editor targets.
# Svg backs the bundled monochrome icon set (Icons::get tinting); it is part
# of the provisioned tree — scripts/setup_qt.py fetches the qtsvg archive.
find_package(Qt6 ${RM_QT_VERSION} QUIET COMPONENTS Widgets OpenGLWidgets Svg)
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

# Sets the deploy-support variables qt_generate_deploy_app_script needs
# (and directory-scope AUTOMOC, which our targets set explicitly anyway).
qt_standard_project_setup()

# Everything testable lives in the lib; the executable stays a thin shell so
# editor tests can link the same objects and run headless (offscreen QPA).
add_library(roadmaker_editor_lib STATIC
  src/app/actions.cpp
  src/app/context_menu.cpp
  src/app/crash_handler.cpp
  src/app/icons.cpp
  src/app/log_setup.cpp
  src/app/main_window.cpp
  src/app/settings.cpp
  src/app/welcome_widget.cpp
  src/document/autosave.cpp
  src/document/diagnostics_model.cpp
  src/document/document.cpp
  src/document/library_list_model.cpp
  src/document/library_manifest.cpp
  src/document/elevation_utils.cpp
  src/document/editor_command.cpp
  src/document/scene_tree_model.cpp
  src/document/selection_model.cpp
  src/panels/diagnostics_panel.cpp
  src/panels/library_panel.cpp
  src/panels/profile_panel.cpp
  src/panels/properties_panel.cpp
  src/panels/scene_tree_panel.cpp
  src/render/gl_functions.cpp
  src/render/gl_renderer.cpp
  src/render/scene_builder.cpp
  src/theme/theme.cpp
  src/tools/create_junction_tool.cpp
  src/tools/create_road_tool.cpp
  src/tools/delete_tool.cpp
  src/tools/edit_nodes_tool.cpp
  src/tools/elevation_tool.cpp
  src/tools/lane_profile_tool.cpp
  src/tools/node_drag.cpp
  src/tools/select_tool.cpp
  src/tools/split_tool.cpp
  src/tools/tool.cpp
  src/tools/tool_manager.cpp
  src/viewport/camera.cpp
  src/viewport/picking.cpp
  src/viewport/viewport_widget.cpp
  resources/resources.qrc
)

# AUTORCC compiles resources.qrc; the lib is static, so icons.cpp pins the
# generated resource object with Q_INIT_RESOURCE (see the comment there).
set_target_properties(roadmaker_editor_lib PROPERTIES AUTOMOC ON AUTORCC ON)

target_include_directories(roadmaker_editor_lib PUBLIC src)

# spdlog is the editor's own logging dependency (not inherited through the
# kernel); Eigen backs the picking math only.
target_link_libraries(roadmaker_editor_lib
  PUBLIC
    roadmaker::core
    Qt6::Widgets
    Qt6::OpenGLWidgets
  PRIVATE
    Qt6::Svg
    Eigen3::Eigen
    spdlog::spdlog
)

rm_apply_warnings(roadmaker_editor_lib)

# Crash reports carry the commit alongside the version (configure-time
# capture — stale only until the next reconfigure, which is fine for a
# best-effort report field).
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE RM_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT RM_GIT_COMMIT)
  set(RM_GIT_COMMIT "unknown")
endif()
target_compile_definitions(roadmaker_editor_lib PRIVATE RM_GIT_COMMIT="${RM_GIT_COMMIT}")

if(WIN32)
  # dbghelp backs SymFromAddr in the SEH crash filter (ships with Windows;
  # not a third-party dependency).
  target_link_libraries(roadmaker_editor_lib PRIVATE dbghelp)
endif()

qt_add_executable(roadmaker-editor src/main.cpp)

target_link_libraries(roadmaker-editor PRIVATE roadmaker_editor_lib)

# GUI app on Windows (no console window) and an .app bundle on macOS — the
# shapes windeployqt/macdeployqt and the installers expect. The macOS binary
# lives at roadmaker-editor.app/Contents/MacOS/roadmaker-editor; `--version`
# smoke tests run that path directly.
set_target_properties(roadmaker-editor PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_BUNDLE_NAME "RoadMaker"
  MACOSX_BUNDLE_GUI_IDENTIFIER "ai.robomous.roadmaker"
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

# Application icon (coral robot). macOS: bundle the .icns into Contents/
# Resources and name it in the Info.plist. Windows: compile the .rc so the
# .ico becomes the executable's Explorer/taskbar icon. Linux gets its icon at
# runtime via QApplication::setWindowIcon plus the .desktop/hicolor install
# rules below.
if(APPLE)
  set(_rm_macos_icon ${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/roadmaker.icns)
  target_sources(roadmaker-editor PRIVATE ${_rm_macos_icon})
  set_source_files_properties(${_rm_macos_icon} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(roadmaker-editor PROPERTIES
    MACOSX_BUNDLE_ICON_FILE roadmaker.icns)
elseif(WIN32)
  target_sources(roadmaker-editor PRIVATE resources/branding/roadmaker.rc)
endif()

rm_apply_warnings(roadmaker-editor)

# Install + Qt runtime deployment must live HERE (not in cmake/Packaging.cmake):
# qt_generate_deploy_app_script reads directory-scoped Qt variables
# (QT_DEPLOY_SUPPORT, QT6_IS_SHARED_LIBS_BUILD) that only exist where
# find_package(Qt6) ran. CPack configuration stays in cmake/Packaging.cmake.
if(RM_INSTALL)
  include(GNUInstallDirs)

  # Linux archive layout: exe in bin/, shared kernel in lib/. (macdeployqt
  # rewrites macOS rpaths itself; Windows finds the DLL beside the exe.)
  if(UNIX AND NOT APPLE)
    set_target_properties(roadmaker-editor PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
  endif()

  install(TARGETS roadmaker-editor
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  # Linux desktop integration: the .desktop entry (app menu / launcher) plus
  # the hicolor icon theme the entry's `Icon=roadmaker` resolves against.
  if(UNIX AND NOT APPLE)
    install(FILES resources/branding/roadmaker.desktop
      DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(_sz IN ITEMS 16 24 32 48 64 128 256 512)
      install(FILES resources/branding/icon_${_sz}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
        RENAME roadmaker.png)
    endforeach()
  endif()

  # macdeployqt deploys Qt only; the shared kernel must be placed into the
  # bundle ourselves. Install the REAL library renamed to its soname (what
  # the executable references) — installing the soname path directly would
  # copy a dangling symlink into the bundle.
  if(APPLE AND RM_BUILD_SHARED)
    install(FILES $<TARGET_FILE:roadmaker_core>
      DESTINATION roadmaker-editor.app/Contents/Frameworks
      RENAME $<TARGET_SONAME_FILE_NAME:roadmaker_core>)
  endif()

  # windeployqt / macdeployqt run during `cmake --install` (and CPack
  # staging), so a local `cpack` produces the same self-contained bundle CI
  # does. Linux deployment is linuxdeploy in the release workflow instead.
  if(WIN32 OR APPLE)
    qt_generate_deploy_app_script(
      TARGET roadmaker-editor
      OUTPUT_SCRIPT _rm_deploy_script
      NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${_rm_deploy_script})
  endif()

  if(APPLE)
    # Ad-hoc signature (no Developer ID yet): Gatekeeper shows the standard
    # unidentified-developer flow instead of refusing a broken signature
    # after macdeployqt rewrites the load commands. Notarization is future.
    install(CODE [[
      execute_process(
        COMMAND codesign --force --deep --sign - "${CMAKE_INSTALL_PREFIX}/roadmaker-editor.app"
        RESULT_VARIABLE _rm_codesign_result)
      if(NOT _rm_codesign_result EQUAL 0)
        message(WARNING "ad-hoc codesign failed (${_rm_codesign_result})")
      endif()
    ]])
  endif()
endif()

if(RM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
