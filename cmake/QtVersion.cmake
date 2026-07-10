# Single source of truth for the Qt version used by the editor.
#
# scripts/setup_qt.py parses the set() line below (and CI keys its Qt cache on
# this file), so keep it machine-readable: exactly `set(RM_QT_VERSION <x.y.z>)`.
set(RM_QT_VERSION 6.8.3)

# If scripts/setup_qt.py provisioned a local Qt under .qt/, prepend it to
# CMAKE_PREFIX_PATH. The install directory name differs from the aqt arch name
# per platform (macos/, gcc_64/, msvc2022_64/), so discover it by glob instead
# of reconstructing it.
file(GLOB _rm_qt_config "${CMAKE_SOURCE_DIR}/.qt/${RM_QT_VERSION}/*/lib/cmake/Qt6")
if(_rm_qt_config)
  list(GET _rm_qt_config 0 _rm_qt_config)
  cmake_path(GET _rm_qt_config PARENT_PATH _rm_qt_prefix) # strip /Qt6
  cmake_path(GET _rm_qt_prefix PARENT_PATH _rm_qt_prefix) # strip /cmake
  cmake_path(GET _rm_qt_prefix PARENT_PATH _rm_qt_prefix) # strip /lib
  list(PREPEND CMAKE_PREFIX_PATH "${_rm_qt_prefix}")
  unset(_rm_qt_prefix)
endif()
unset(_rm_qt_config)
