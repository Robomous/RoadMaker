# GNUInstallDirs must be included before CMAKE_INSTALL_INCLUDEDIR is baked
# into the INSTALL_INTERFACE include path below.
include(GNUInstallDirs)

set(_rm_core_type STATIC)
if(RM_BUILD_SHARED)
  set(_rm_core_type SHARED)
endif()

add_library(roadmaker_core ${_rm_core_type}
  src/version.cpp
  src/geometry/reference_line.cpp
  src/geometry/profile_fit.cpp
  src/geometry/road_intersection.cpp
  src/road/network.cpp
  src/road/authoring.cpp
  src/road/surface_derivation.cpp
  src/edit/edit_stack.cpp
  src/edit/connection.cpp
  src/edit/markings.cpp
  src/edit/operations.cpp
  src/edit/snap.cpp
  src/xodr/reader.cpp
  src/xodr/writer.cpp
  src/xodr/junction_export.cpp
  src/mesh/mesh_builder.cpp
  src/mesh/junction_surface.cpp
  src/mesh/surface_fill.cpp
  src/assets/prop_meshes.gen.cpp
  src/io/gltf_exporter.cpp
)
add_library(roadmaker::core ALIAS roadmaker_core)

# OpenUSD (.usda) exporter — optional, gated on RM_BUILD_USD. RM_HAVE_USD is
# PUBLIC so consumers (bindings, editor) can gate on the exported symbol.
if(RM_BUILD_USD)
  target_sources(roadmaker_core PRIVATE src/io/usd_exporter.cpp)
  target_link_libraries(roadmaker_core PRIVATE tinyusdz::tinyusdz)
  target_compile_definitions(roadmaker_core PUBLIC RM_HAVE_USD)
endif()

# Explicit RM_API exports only — hidden visibility everywhere else. A missed
# annotation is a LINK error in the shared CI job, which is the point:
# WINDOWS_EXPORT_ALL_SYMBOLS would paper over API-hygiene mistakes.
include(GenerateExportHeader)
generate_export_header(roadmaker_core
  BASE_NAME RM
  EXPORT_MACRO_NAME RM_API
  EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/roadmaker/export.hpp
)
if(NOT RM_BUILD_SHARED)
  target_compile_definitions(roadmaker_core PUBLIC RM_STATIC_DEFINE)
endif()

set_target_properties(roadmaker_core PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  # Pre-1.0 convention: every minor is a new ABI epoch.
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
  # Installed consumers link the same spelling the build tree uses.
  EXPORT_NAME core
)

target_include_directories(roadmaker_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(roadmaker_core PUBLIC cxx_std_20)

# tl::expected is the ONLY public dependency (rm::Expected in error.hpp).
# BUILD_INTERFACE-only: install(EXPORT) would otherwise resolve the name to
# our non-exported FetchContent alias and refuse to generate. The installed
# package re-attaches the dependency in roadmakerConfig.cmake via
# find_dependency(tl-expected). Everything else — including fmt/spdlog/Eigen,
# which appear in NO public header — is PRIVATE and, in shared builds, fully
# hidden inside the library.
target_link_libraries(roadmaker_core
  PUBLIC
    "$<BUILD_INTERFACE:tl::expected>"
  PRIVATE
    Eigen3::Eigen
    fmt::fmt
    spdlog::spdlog
    FastFloat::fast_float
    Clothoids::Clothoids
    pugixml::pugixml
    Clipper2::Clipper2
    CDT::CDT
    manifold::manifold
    tinygltf::tinygltf
)

# Single source of truth for the version string returned by rm::version().
set_source_files_properties(src/version.cpp PROPERTIES
  COMPILE_DEFINITIONS RM_VERSION_STRING="${PROJECT_VERSION}")

rm_apply_warnings(roadmaker_core)

# The installable CMake package ships the SHARED kernel only: a static
# roadmaker_core keeps its PRIVATE FetchContent deps in the link interface
# ($<LINK_ONLY:...>), which install(EXPORT) rightly refuses to export.
if(RM_INSTALL AND RM_BUILD_SHARED)
  include(CMakePackageConfigHelpers)

  install(TARGETS roadmaker_core EXPORT roadmakerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(DIRECTORY include/roadmaker DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/roadmaker/export.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/roadmaker)
  install(EXPORT roadmakerTargets
    NAMESPACE roadmaker::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/roadmaker)

  configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/roadmakerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/roadmakerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/roadmaker)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/roadmakerConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/roadmakerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/roadmakerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/roadmaker)
endif()

if(RM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
