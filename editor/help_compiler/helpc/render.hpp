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

// Markdown → HTML for one guide page, with the link/image rewrites the offline
// Qt Help collection needs. Qt-free (md4c-html only).

#include <filesystem>
#include <string>

namespace roadmaker::helpc {

struct RenderOptions {
  std::string title;                 ///< page H1, used for <title>
  std::string css_href = "help.css"; ///< stylesheet the page links

  /// `../foo.md` links leave the guide, so they cannot be served from the
  /// collection; they are rewritten to the page on GitHub. `guide_rel` is the
  /// guide directory relative to the repo root, so `../` normalises correctly.
  std::string repo_blob_base = "https://github.com/Robomous/RoadMaker/blob/main";
  std::string guide_rel = "docs/user-guide";

  /// When both are set, images referenced with a `../` prefix (i.e. outside the
  /// guide dir) are copied into `img_out_dir` and rewritten to `img/<name>`.
  /// `guide_dir` resolves the source; leave empty to skip image copying.
  std::filesystem::path guide_dir;
  std::filesystem::path img_out_dir;
};

/// Render `markdown` to a full standalone HTML document.
[[nodiscard]] std::string render_page(const std::string& markdown, const RenderOptions& opts);

/// Rewrite a single href/src target per the collection rules. Exposed for
/// tests; `copied_image` (out) receives the source path of any external image
/// that should be copied, or stays empty.
[[nodiscard]] std::string rewrite_target(const std::string& target,
                                         const RenderOptions& opts,
                                         bool is_image,
                                         std::string& copied_image);

} // namespace roadmaker::helpc
