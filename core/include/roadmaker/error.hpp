// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
