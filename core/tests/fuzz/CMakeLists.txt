# libFuzzer targets — Clang only (RM_BUILD_FUZZERS=ON). The CI fuzz job runs
# a short smoke of each; longer campaigns are run manually:
#   ./build/core/tests/fuzz/roadmaker_fuzz_xodr core/tests/fuzz/corpus -max_total_time=60
#   ./build/core/tests/fuzz/roadmaker_fuzz_xosc core/tests/fuzz/corpus_xosc -max_total_time=60
#
# One target per PARSER, each with its own corpus directory: the two formats
# share no grammar, so a mixed corpus makes both campaigns spend their budget
# rediscovering that the other standard's root element is not theirs.
foreach(parser xodr xosc)
  add_executable(roadmaker_fuzz_${parser} fuzz_${parser}.cpp)
  target_link_libraries(roadmaker_fuzz_${parser} PRIVATE roadmaker::core)
  target_compile_options(roadmaker_fuzz_${parser} PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(roadmaker_fuzz_${parser} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
