// tinygltf implementation TU for the test binary — needed ONLY when the
// kernel is a shared library: core compiles its own copy inside
// gltf_exporter.cpp, but hidden visibility keeps those symbols internal, so
// test_gltf.cpp's direct tinygltf usage (glb re-load validation) must bring
// its own. Never compiled in static builds (duplicate definitions against
// the core archive). Config macros come from the rm_tinygltf CMake target.
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
