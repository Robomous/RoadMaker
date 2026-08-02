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
#include <optional>
#include <string>

namespace roadmaker::helpc {

/// The reference→guide bridge (ADR-0009 / docs-s2). A reference page ends with a
/// section under this exact heading whose first link points at the full guide.
///
/// The heading is the marker, so the AUTHORED link stays an ordinary relative
/// Markdown link — it renders correctly on GitHub and the site adapter turns it
/// into a normal site link. Only this pipeline rewrites it, because only this
/// pipeline needs a URL the in-app viewer can recognise.
inline constexpr const char* kBridgeHeading = "## Full guide";

/// Scheme the rewritten bridge link carries. `HelpBrowser` resolves it against
/// the packaged manual at runtime and opens it in the external browser; nothing
/// else in the collection uses it. It survives `rewrite_target` untouched, being
/// neither `../`-prefixed nor `.md`-suffixed.
inline constexpr const char* kManualScheme = "rmmanual:";

/// One page's bridge link, as authored and as resolved.
struct BridgeLink {
  std::string text;   ///< the link's label
  std::string target; ///< exactly as authored, e.g. `../tutorials/getting-around.md`
  std::string slug;   ///< guide-relative, extensionless, e.g. `tutorials/getting-around`
  std::string anchor; ///< `#fragment`, or empty

  /// Byte range of `target` in the source. The renderer rewrites AT this offset
  /// rather than searching for the text: a page may well link the same guide
  /// earlier in its prose, and only the one in the bridge section may change.
  std::size_t target_begin = 0;
  std::size_t target_end = 0;
};

struct RenderOptions {
  std::string title;                 ///< page H1, used for <title>
  std::string css_href = "help.css"; ///< stylesheet the page links

  /// This page's path relative to the guide dir (`reference/junction.md`). Used
  /// ONLY to resolve the bridge link's slug; the general link rewriting below is
  /// deliberately left as it was (see #297).
  std::string page_rel;

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

/// The bridge link in `markdown`'s `## Full guide` section, or nullopt when the
/// page has no such section. `page_rel` is the page's path relative to the guide
/// directory, which is what makes `../tutorials/x.md` resolve to `tutorials/x`.
///
/// Shared by the renderer and by the gate that proves every bridge target is a
/// page that exists — one parser, so the gate cannot check a different thing
/// from the one that ships.
[[nodiscard]] std::optional<BridgeLink> bridge_link(const std::string& markdown,
                                                    const std::string& page_rel);

/// Rewrite a single href/src target per the collection rules. Exposed for
/// tests; `copied_image` (out) receives the source path of any external image
/// that should be copied, or stays empty.
[[nodiscard]] std::string rewrite_target(const std::string& target,
                                         const RenderOptions& opts,
                                         bool is_image,
                                         std::string& copied_image);

} // namespace roadmaker::helpc
