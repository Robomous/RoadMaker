// libFuzzer entry point for the OpenDRIVE parser. The parser must never
// crash, hang, or leak on arbitrary input — it may only return errors and
// diagnostics. Build with RM_BUILD_FUZZERS=ON (Clang only).

#include "roadmaker/xodr/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto result = roadmaker::parse_xodr(text, "<fuzz>");
  (void)result;
  return 0;
}
