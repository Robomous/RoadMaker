add_library(roadmaker_core STATIC
  src/version.cpp
)
add_library(roadmaker::core ALIAS roadmaker_core)

target_include_directories(roadmaker_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(roadmaker_core PUBLIC cxx_std_20)

target_link_libraries(roadmaker_core
  PUBLIC
    Eigen3::Eigen
    fmt::fmt
    spdlog::spdlog
  PRIVATE
    Clothoids::Clothoids
    pugixml::pugixml
    Clipper2::Clipper2
    CDT::CDT
    manifold::manifold
    tinygltf::tinygltf
)

rm_apply_warnings(roadmaker_core)

if(RM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
