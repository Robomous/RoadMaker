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

#pragma once

#include <cstddef>
#include <optional>

#if defined(__linux__)
#include <cstdio>
#include <cstring>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
// WIN32_LEAN_AND_MEAN and NOMINMAX keep <windows.h> from dragging in the RPC
// headers and the min/max macros. It still defines `small` as `char` (from
// rpcndr.h's IDL support), which turned `const double small = ...` in the
// bench into "'double' followed by 'char' is illegal" — so that one is undef'd
// by name rather than hoped away, because any translation unit that includes
// this header inherits it.
// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#undef small
// clang-format on
#endif

namespace roadmaker::scale {

/// Peak resident set size in bytes, or nullopt where the platform gives no
/// answer this build knows how to ask for.
///
/// All three platforms are implemented — but the scale bench GATES on Linux
/// only, and says so. A ceiling calibrated against glibc's allocator is not a
/// ceiling against the macOS zone allocator or the Windows low-fragmentation
/// heap, and pretending otherwise produces either a flaky gate or a
/// meaningless one. The allocator-independent number the bench prints beside
/// this — the mesh's own byte accounting — is what a regression should be read
/// from.
[[nodiscard]] inline std::optional<std::size_t> peak_rss_bytes() {
#if defined(__linux__)
  // VmHWM is the kernel's own high-water mark, so it survives the process
  // having already freed the peak.
  std::FILE* status = std::fopen("/proc/self/status", "re");
  if (status == nullptr) {
    return std::nullopt;
  }
  char line[256];
  std::optional<std::size_t> peak;
  while (std::fgets(line, sizeof(line), status) != nullptr) {
    unsigned long kilobytes = 0;
    if (std::sscanf(line, "VmHWM: %lu kB", &kilobytes) == 1) {
      peak = static_cast<std::size_t>(kilobytes) * 1024U;
      break;
    }
  }
  std::fclose(status);
  return peak;
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(
          mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(info.resident_size_max);
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(counters.PeakWorkingSetSize);
#else
  return std::nullopt;
#endif
}

/// Whether this platform's peak RSS is one the bench is willing to GATE on.
[[nodiscard]] inline constexpr bool peak_rss_is_gateable() {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

} // namespace roadmaker::scale
