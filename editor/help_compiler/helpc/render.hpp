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
