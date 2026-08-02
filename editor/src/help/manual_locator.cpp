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

#include "help/manual_locator.hpp"

#include <QCoreApplication>
#include <system_error>

namespace roadmaker::editor::help {

namespace {

constexpr const char* kIndexName = "index.html";

} // namespace

ManualPlatform this_platform() {
#if defined(Q_OS_MACOS)
  return ManualPlatform::kMacOS;
#elif defined(Q_OS_WIN)
  return ManualPlatform::kWindows;
#else
  return ManualPlatform::kLinux;
#endif
}

std::filesystem::path manual_dir_for(const std::filesystem::path& exe_dir,
                                     ManualPlatform platform) {
  switch (platform) {
  case ManualPlatform::kMacOS:
    // .../RoadMaker.app/Contents/MacOS -> .../Contents/Resources/manual
    return (exe_dir / ".." / "Resources" / "manual").lexically_normal();
  case ManualPlatform::kLinux:
    // The archive puts the executable in bin/ and shared data under share/.
    return (exe_dir / ".." / "share" / "roadmaker" / "manual").lexically_normal();
  case ManualPlatform::kWindows:
    break;
  }
  return (exe_dir / "manual").lexically_normal();
}

std::filesystem::path manual_dir() {
  const std::filesystem::path exe_dir(QCoreApplication::applicationDirPath().toStdString());
  return manual_dir_for(exe_dir, this_platform());
}

std::optional<std::filesystem::path> manual_index() {
  const std::filesystem::path index = manual_dir() / kIndexName;
  std::error_code ec;
  if (!std::filesystem::exists(index, ec) || ec) {
    return std::nullopt;
  }
  return index;
}

std::optional<std::filesystem::path> manual_page_for(const std::filesystem::path& manual_root,
                                                     const std::string& slug) {
  if (slug.empty()) {
    return std::nullopt;
  }
  // A backslash would be a directory separator on Windows only, so the same slug
  // would mean two different things; reject rather than normalise.
  if (slug.find('\\') != std::string::npos || slug.front() == '/') {
    return std::nullopt;
  }

  const std::filesystem::path relative = std::filesystem::path(slug).lexically_normal();
  for (const std::filesystem::path& part : relative) {
    if (part == "..") {
      return std::nullopt;
    }
  }

  std::filesystem::path page = manual_root / relative;
  page += ".html";
  return page.lexically_normal();
}

} // namespace roadmaker::editor::help
