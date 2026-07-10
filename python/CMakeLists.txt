# Two entry modes:
#  1. scikit-build-core builds this directory as the top-level project
#     (pip install / pip wheel) — we pull the kernel in via add_subdirectory.
#  2. The repo superbuild adds this directory when RM_BUILD_PYTHON=ON —
#     roadmaker::core already exists.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.26)
  project(roadmaker_python LANGUAGES CXX C)

  set(RM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(RM_BUILD_PYTHON OFF CACHE BOOL "" FORCE) # guard against recursion
  set(RM_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/roadmaker_kernel)
endif()

find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

# nanobind is a build requirement (pyproject.toml); locate its CMake config
# through the interpreter that is driving the build.
execute_process(
  COMMAND ${Python_EXECUTABLE} -m nanobind --cmake_dir
  OUTPUT_VARIABLE NB_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
list(APPEND CMAKE_PREFIX_PATH ${NB_DIR})
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_roadmaker src/bindings.cpp)
target_link_libraries(_roadmaker PRIVATE roadmaker::core)

install(TARGETS _roadmaker LIBRARY DESTINATION roadmaker)
