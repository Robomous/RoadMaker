add_executable(roadmaker_core_tests
  test_smoke.cpp
  test_id_arena.cpp
  test_network.cpp
)

target_link_libraries(roadmaker_core_tests
  PRIVATE
    roadmaker::core
    Clothoids::Clothoids
    CDT::CDT
    Clipper2::Clipper2
    manifold::manifold
    pugixml::pugixml
    tinygltf::tinygltf
    Catch2::Catch2WithMain
)

rm_apply_warnings(roadmaker_core_tests)

catch_discover_tests(roadmaker_core_tests)
