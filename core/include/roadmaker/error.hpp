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

#include <string>
#include <tl/expected.hpp>
#include <utility>

namespace roadmaker {

enum class ErrorCode {
  FileNotFound,
  MalformedXml,
  InvalidDocument, // structurally not an OpenDRIVE document
  InvalidArgument,
  IoFailure,
};

/// Rich error payload carried across the public kernel API instead of
/// exceptions. `context` names the thing that failed (a path, an element
/// location, an id) — keep `message` human-readable.
struct Error {
  ErrorCode code = ErrorCode::InvalidArgument;
  std::string message;
  std::string context;
};

/// Kernel-wide expected alias. std::expected is C++23; the kernel targets
/// C++20, so this pins tl::expected with an identical interface.
template <class T>
using Expected = tl::expected<T, Error>;

template <class... Args>
[[nodiscard]] inline tl::unexpected<Error>
make_error(ErrorCode code, std::string message, std::string context = {}) {
  return tl::unexpected<Error>(
      Error{.code = code, .message = std::move(message), .context = std::move(context)});
}

} // namespace roadmaker
