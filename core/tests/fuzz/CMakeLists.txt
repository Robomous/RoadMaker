# libFuzzer target — Clang only (RM_BUILD_FUZZERS=ON). The CI fuzz job runs
# a short smoke; longer campaigns are run manually:
#   ./build/core/tests/fuzz/roadmaker_fuzz_xodr core/tests/fuzz/corpus -max_total_time=60
add_executable(roadmaker_fuzz_xodr fuzz_xodr.cpp)
target_link_libraries(roadmaker_fuzz_xodr PRIVATE roadmaker::core)
target_compile_options(roadmaker_fuzz_xodr PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(roadmaker_fuzz_xodr PRIVATE -fsanitize=fuzzer,address,undefined)
