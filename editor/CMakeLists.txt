# Qt is provisioned locally by scripts/setup_qt.py (never FetchContent — too
# large) and discovered via cmake/QtVersion.cmake. LGPLv3, dynamic linking
# only, confined to editor targets.
# Svg backs the bundled monochrome icon set (Icons::get tinting); it is part
# of the provisioned tree — scripts/setup_qt.py fetches the qtsvg archive.
# Help provides the QHelpEngine the in-app viewer serves the guide from; Tools
# provides qhelpgenerator (an IMPORTED target, Qt6::qhelpgenerator), which
# compiles the .qhp/.qhcp into the shipped .qch/.qhc at build time. Both ride
# in qttools, provisioned by scripts/setup_qt.py.
find_package(Qt6 ${RM_QT_VERSION} QUIET COMPONENTS Widgets OpenGLWidgets Svg Help Tools)
if(NOT Qt6_FOUND)
  message(FATAL_ERROR
    "Qt ${RM_QT_VERSION} not found. Run `python3 scripts/setup_qt.py` to "
    "provision it into ./.qt/, then re-configure.")
endif()

# qhelpgenerator is an IMPORTED executable in Qt6ToolsTools — using it abstracts
# over its libexec/ (macOS) vs bin/ (Windows/Linux) location. A tree missing it
# was provisioned before qttools carried the Help module.
if(NOT TARGET Qt6::qhelpgenerator)
  message(FATAL_ERROR
    "Qt6::qhelpgenerator not found (the qttools Help module is missing). "
    "Re-run `python3 scripts/setup_qt.py` to re-provision Qt into ./.qt/.")
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

# The (Qt-free) help compiler: rm_helpc + its testable helpc_lib.
add_subdirectory(help_compiler)

# Everything testable lives in the lib; the executable stays a thin shell so
# editor tests can link the same objects and run headless (offscreen QPA).
add_library(roadmaker_editor_lib STATIC
  src/app/actions.cpp
  src/app/shortcut_registry.cpp
  src/app/context_menu.cpp
  src/app/crash_handler.cpp
  src/app/icons.cpp
  src/app/log_setup.cpp
  src/app/main_window.cpp
  src/app/settings.cpp
  src/app/tour_controller.cpp
  src/app/tour_overlay.cpp
  src/app/welcome_widget.cpp
  src/document/autosave.cpp
  src/document/diagnostics_model.cpp
  src/document/document.cpp
  src/document/library_drop.cpp
  src/document/library_list_model.cpp
  src/document/library_manifest.cpp
  src/document/project.cpp
  src/document/elevation_utils.cpp
  src/document/editor_command.cpp
  src/document/scene_tree_model.cpp
  src/document/selection_model.cpp
  src/help/help_browser.cpp
  src/help/help_locator.cpp
  src/help/help_registry.cpp
  src/help/help_viewer.cpp
  src/panels/diagnostics_panel.cpp
  src/panels/editor2d_host.cpp
  src/panels/library_panel.cpp
  src/panels/profile_panel.cpp
  src/panels/properties_panel.cpp
  src/panels/scene_tree_panel.cpp
  src/panels/scrub_label.cpp
  src/panels/slot_widget.cpp
  src/panels/width_panel.cpp
  src/render/gl_functions.cpp
  src/render/gl_renderer.cpp
  src/render/scene_builder.cpp
  src/theme/help_style.cpp
  src/theme/theme.cpp
  src/tools/create_junction_tool.cpp
  src/tools/create_road_tool.cpp
  src/tools/delete_tool.cpp
  src/tools/edit_nodes_tool.cpp
  src/tools/elevation_tool.cpp
  src/tools/lane_add_tool.cpp
  src/tools/lane_carve_tool.cpp
  src/tools/lane_form_tool.cpp
  src/tools/lane_profile_tool.cpp
  src/tools/node_drag.cpp
  src/tools/select_tool.cpp
  src/tools/split_tool.cpp
  src/tools/tool.cpp
  src/tools/tool_manager.cpp
  src/viewport/camera.cpp
  src/viewport/framing.cpp
  src/viewport/gizmo.cpp
  src/viewport/nav_controller.cpp
  src/viewport/picking.cpp
  src/viewport/toast_queue.cpp
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
    Qt6::Help
    Eigen3::Eigen
    spdlog::spdlog
)

# The in-app help viewer versions its writable collection copy by this string.
target_compile_definitions(roadmaker_editor_lib PRIVATE RM_HELP_VERSION="${PROJECT_VERSION}")

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

# ---------------------------------------------------------------------------
# Qt Help pipeline: compile the committed Markdown guide into a .qch/.qhc
# collection at build time, then stage it into the app. rm_helpc renders the
# guide to HTML + .qhp + .qhcp; Qt6::qhelpgenerator compiles those into the
# binary collection. The GLOB with CONFIGURE_DEPENDS means adding a guide page
# needs no CMake edit — CMake re-runs when the glob result changes. The output
# lives under the build tree only; nothing here is committed.
set(_rm_help_out ${CMAKE_BINARY_DIR}/help)
set(_rm_guide_dir ${CMAKE_SOURCE_DIR}/docs/user-guide)
set(_rm_help_css ${CMAKE_CURRENT_SOURCE_DIR}/resources/help/help.css)

file(GLOB _rm_help_inputs CONFIGURE_DEPENDS
  ${_rm_guide_dir}/*.md
  ${_rm_guide_dir}/tutorials/*.md
  ${_rm_guide_dir}/img/*
  ${_rm_guide_dir}/tutorials/img/*)

set(_rm_qhp ${_rm_help_out}/html/roadmaker.qhp)
set(_rm_qhcp ${_rm_help_out}/roadmaker.qhcp)
set(_rm_qch ${_rm_help_out}/roadmaker.qch)
set(_rm_qhc ${_rm_help_out}/roadmaker.qhc)

# 1. Markdown -> HTML + .qhp + .qhcp
add_custom_command(
  OUTPUT ${_rm_qhp} ${_rm_qhcp}
  COMMAND rm_helpc
          --guide ${_rm_guide_dir}
          --css ${_rm_help_css}
          --out ${_rm_help_out}
          --namespace ai.robomous.roadmaker
          --folder doc
          --version ${PROJECT_VERSION}
  DEPENDS rm_helpc ${_rm_help_inputs} ${_rm_help_css}
  COMMENT "Compiling user guide -> HTML/.qhp (rm_helpc)"
  VERBATIM)

# 2. .qhp -> .qch
add_custom_command(
  OUTPUT ${_rm_qch}
  COMMAND Qt6::qhelpgenerator ${_rm_qhp} -o ${_rm_qch}
  DEPENDS ${_rm_qhp}
  COMMENT "Generating roadmaker.qch (qhelpgenerator)"
  VERBATIM)

# 3. .qhcp -> .qhc (registers the .qch; the two ship side by side)
add_custom_command(
  OUTPUT ${_rm_qhc}
  COMMAND Qt6::qhelpgenerator ${_rm_qhcp} -o ${_rm_qhc}
  DEPENDS ${_rm_qhcp} ${_rm_qch}
  COMMENT "Generating roadmaker.qhc (qhelpgenerator)"
  VERBATIM)

add_custom_target(roadmaker_help ALL DEPENDS ${_rm_qch} ${_rm_qhc})

# Build the collection before the app so the POST_BUILD staging has it.
add_dependencies(roadmaker-editor roadmaker_help)

# Stage the collection into the app: macOS bundle Resources, or a help/ dir
# beside the binary on Windows/Linux.
if(APPLE)
  set(_rm_app_help_dir $<TARGET_BUNDLE_CONTENT_DIR:roadmaker-editor>/Resources/help)
else()
  set(_rm_app_help_dir $<TARGET_FILE_DIR:roadmaker-editor>/help)
endif()
add_custom_command(TARGET roadmaker-editor POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_rm_app_help_dir}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_rm_qch} ${_rm_qhc} ${_rm_app_help_dir}
  COMMENT "Staging help collection into the app")

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

  # Ship the help collection beside the binary (Win/Linux; the AppImage
  # inherits it). macOS carries it inside the bundle via the POST_BUILD stage
  # above, which `BUNDLE DESTINATION .` copies wholesale.
  if(NOT APPLE)
    install(FILES ${_rm_qch} ${_rm_qhc} DESTINATION bin/help)
  endif()

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
