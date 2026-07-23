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

// Table of contents for the user guide, derived from docs/user-guide/index.md.
//
// The help compiler is deliberately Qt-free (std::filesystem + md4c only) so it
// builds as a host tool with nothing to deploy. This header is the contract the
// renderer, the .qhp writer, and the tests share.

#include <filesystem>
#include <string>
#include <vector>

namespace roadmaker::helpc {

/// One guide page in the table of contents.
struct TocEntry {
  std::string rel_path;  ///< path relative to the guide dir, e.g. "create-road.md"
  std::string slug;      ///< rel_path minus ".md" — the help-s2 keyword id contract
  std::string title;     ///< the page's H1
  bool tutorial = false; ///< true when it nests under the synthetic "Tutorials" node
};

/// The whole guide: the index page plus every page it links, in link order.
struct Toc {
  TocEntry index;              ///< index.md (slug "index")
  std::vector<TocEntry> pages; ///< linked pages, in the order index.md lists them
};

/// Build the table of contents from `guide_dir`/index.md.
///
/// Follows the order of index.md's markdown links; drops links that are
/// external, that escape the guide directory (a `../` prefix), that are not
/// `.md`, or whose target file does not exist. Each page's title is its H1;
/// `tutorials/` pages are flagged for the synthetic Tutorials node.
[[nodiscard]] Toc build_toc(const std::filesystem::path& guide_dir);

/// The index page followed by every linked page — the iteration order for
/// keyword emission (every page gets exactly one keyword).
[[nodiscard]] std::vector<TocEntry> all_pages(const Toc& toc);

/// The first `# ` heading in `markdown`, trimmed; empty when there is none.
[[nodiscard]] std::string first_h1(const std::string& markdown);

/// Ordered, de-duplicated markdown link targets in `markdown`, excluding image
/// links. Anchors are preserved on each target.
[[nodiscard]] std::vector<std::string> extract_links(const std::string& markdown);

/// Ordered markdown image targets (`![alt](target)`) in `markdown`, verbatim.
/// The coverage tests use this to assert every referenced image is shipped.
[[nodiscard]] std::vector<std::string> extract_image_links(const std::string& markdown);

} // namespace roadmaker::helpc
