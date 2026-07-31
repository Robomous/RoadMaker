/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// libFuzzer entry point for the OpenSCENARIO parser (p8-s1, issue #245), the
// twin of fuzz_xodr.cpp and held to the same contract: the parser must never
// crash, hang, or leak on arbitrary input — it may only return errors and
// diagnostics. Build with RM_BUILD_FUZZERS=ON (Clang only).
//
// Its own corpus directory, not the OpenDRIVE one: seeding a scenario parser
// with road networks spends the whole campaign re-discovering that
// <OpenDRIVE> is not <OpenSCENARIO>.

#include "roadmaker/osc/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto result = roadmaker::osc::parse_xosc(text, "<fuzz>");
  (void)result;
  return 0;
}
