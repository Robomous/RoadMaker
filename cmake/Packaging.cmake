# Editor packaging: self-contained bundles per platform, the "user installs
# nothing" deliverable. Included from the root CMakeLists only when
# RM_BUILD_EDITOR AND RM_INSTALL.
#
#   Windows  windeployqt at install time -> CPack NSIS installer + portable ZIP
#   macOS    macdeployqt at install time + ad-hoc codesign -> CPack DragNDrop DMG
#   Linux    CPack TGZ here; the AppImage is produced by linuxdeploy in the
#            release workflow (qt_generate_deploy_app_script has no Linux
#            support, and linuxdeploy is a CI-friendly AppImage tool that does
#            not belong inside install(CODE)).
#
# Every bundle carries the MIT license, THIRD_PARTY_LICENSES.md, and the Qt
# LGPLv3 texts: Qt stays a set of replaceable shared libraries (LGPL relink
# provision — see THIRD_PARTY_LICENSES.md).

# Target install rules + windeployqt/macdeployqt live in editor/CMakeLists.txt
# (they need the editor directory's Qt variable scope); this file is CPack
# configuration and shared documentation files only.

set(_rm_doc_dir ${CMAKE_INSTALL_DATADIR}/doc/roadmaker)
if(APPLE)
  set(_rm_doc_dir licenses) # inside the DMG, next to the .app
endif()
install(FILES
  ${CMAKE_SOURCE_DIR}/LICENSE
  ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
  ${CMAKE_SOURCE_DIR}/licenses/LGPL-3.0-only.txt
  ${CMAKE_SOURCE_DIR}/licenses/GPL-3.0-only.txt
  DESTINATION ${_rm_doc_dir}
)

set(CPACK_PACKAGE_NAME "roadmaker")
set(CPACK_PACKAGE_VENDOR "Robomous")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL})
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_FILE_NAME
    "roadmaker-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)

if(WIN32)
  set(CPACK_GENERATOR "NSIS;ZIP")
  set(CPACK_NSIS_DISPLAY_NAME "RoadMaker")
  set(CPACK_NSIS_PACKAGE_NAME "RoadMaker")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ${CMAKE_INSTALL_BINDIR})
  set(CPACK_PACKAGE_EXECUTABLES "roadmaker-editor" "RoadMaker")
elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
