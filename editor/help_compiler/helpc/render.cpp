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

#include "helpc/render.hpp"

#include <regex>
#include <system_error>

#include "md4c-html.h"

namespace roadmaker::helpc {

namespace {

void collect(const MD_CHAR* text, MD_SIZE size, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(text, size);
}

std::string md_to_html(const std::string& markdown) {
  std::string html;
  md_html(markdown.data(),
          static_cast<MD_SIZE>(markdown.size()),
          &collect,
          &html,
          MD_DIALECT_GITHUB,
          0);
  return html;
}

bool is_absolute_link(const std::string& target) {
  return target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 ||
         target.rfind("mailto:", 0) == 0 || (!target.empty() && target[0] == '#');
}

std::pair<std::string, std::string> split_anchor(const std::string& target) {
  const auto hash = target.find('#');
  if (hash == std::string::npos) {
    return {target, {}};
  }
  return {target.substr(0, hash), target.substr(hash)};
}

/// The span of the `## Full guide` section, or npos when the page has none.
/// The heading must be alone on its line; the section runs to the next H2.
std::pair<std::size_t, std::size_t> bridge_span(const std::string& markdown) {
  const std::string heading = kBridgeHeading;
  std::size_t at = 0;
  while ((at = markdown.find(heading, at)) != std::string::npos) {
    const bool line_start = at == 0 || markdown[at - 1] == '\n';
    const std::size_t after = at + heading.size();
    const bool line_end =
        after >= markdown.size() || markdown[after] == '\n' || markdown[after] == '\r';
    if (line_start && line_end) {
      const std::size_t next = markdown.find("\n## ", after);
      return {after, next == std::string::npos ? markdown.size() : next};
    }
    at = after;
  }
  return {std::string::npos, std::string::npos};
}

} // namespace

std::optional<BridgeLink> bridge_link(const std::string& markdown, const std::string& page_rel) {
  const auto [begin, end] = bridge_span(markdown);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  // The FIRST markdown link in the section is the bridge; anything after it is
  // ordinary prose.
  const std::size_t open = markdown.find('[', begin);
  if (open == std::string::npos || open >= end) {
    return std::nullopt;
  }
  const std::size_t close = markdown.find("](", open);
  if (close == std::string::npos || close >= end) {
    return std::nullopt;
  }
  const std::size_t paren = markdown.find(')', close + 2);
  if (paren == std::string::npos || paren >= end) {
    return std::nullopt;
  }

  BridgeLink link;
  link.text = markdown.substr(open + 1, close - (open + 1));
  link.target_begin = close + 2;
  link.target_end = paren;
  link.target = markdown.substr(link.target_begin, link.target_end - link.target_begin);

  std::string path = link.target;
  if (const auto hash = path.find('#'); hash != std::string::npos) {
    link.anchor = path.substr(hash);
    path = path.substr(0, hash);
  }
  if (path.empty() || is_absolute_link(path)) {
    return std::nullopt;
  }

  // Resolve against the PAGE's directory, which is what turns a reference page's
  // `../tutorials/x.md` into the guide-relative `tutorials/x`.
  std::filesystem::path resolved =
      std::filesystem::path(page_rel).parent_path() / std::filesystem::path(path);
  resolved = resolved.lexically_normal();
  std::string slug = resolved.generic_string();
  if (slug.size() >= 3 && slug.substr(slug.size() - 3) == ".md") {
    slug = slug.substr(0, slug.size() - 3);
  }
  link.slug = slug;
  return link;
}

std::string rewrite_target(const std::string& target,
                           const RenderOptions& opts,
                           bool is_image,
                           std::string& copied_image) {
  copied_image.clear();
  if (target.empty() || is_absolute_link(target)) {
    return target;
  }

  const auto [path, anchor] = split_anchor(target);

  // A `../` prefix leaves the guide directory.
  if (path.rfind("../", 0) == 0) {
    if (is_image) {
      // Copy the out-of-guide image into the collection's img/ dir and point at
      // it locally; the caller (render_page) performs the copy.
      copied_image = path;
      const std::filesystem::path name = std::filesystem::path(path).filename();
      return "img/" + name.generic_string();
    }
    std::filesystem::path resolved = std::filesystem::path(opts.guide_rel) / path;
    resolved = resolved.lexically_normal();
    return opts.repo_blob_base + "/" + resolved.generic_string() + anchor;
  }

  // In-guide markdown page: serve the rendered .html.
  if (!is_image && path.size() >= 3 && path.substr(path.size() - 3) == ".md") {
    return path.substr(0, path.size() - 3) + ".html" + anchor;
  }
  return target;
}

std::string render_page(const std::string& markdown, const RenderOptions& opts) {
  std::string source = markdown;

  // Retarget the bridge link BEFORE the Markdown is rendered, so the generic
  // href rewriting below never sees it. `rmmanual:` reaches the viewer intact.
  if (const std::optional<BridgeLink> bridge = bridge_link(source, opts.page_rel)) {
    source.replace(bridge->target_begin,
                   bridge->target_end - bridge->target_begin,
                   std::string(kManualScheme) + bridge->slug + bridge->anchor);
  }

  std::string body = md_to_html(source);

  const auto rewrite_attr = [&](const std::string& attr, bool is_image) {
    const std::regex pattern(attr + R"rx(="([^"]*)")rx");
    std::string result;
    const auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), pattern); it != end; ++it) {
      const std::smatch& match = *it;
      const std::size_t pos = static_cast<std::size_t>(match.position());
      const std::size_t len = static_cast<std::size_t>(match.length());
      result.append(body, last, pos - last);
      std::string copied;
      const std::string rewritten = rewrite_target(match[1].str(), opts, is_image, copied);
      if (!copied.empty() && !opts.guide_dir.empty() && !opts.img_out_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opts.img_out_dir, ec);
        const std::filesystem::path src = (opts.guide_dir / copied).lexically_normal();
        const std::filesystem::path dst =
            opts.img_out_dir / std::filesystem::path(copied).filename();
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
      }
      result.append(attr + "=\"" + rewritten + "\"");
      last = pos + len;
    }
    result.append(body, last, std::string::npos);
    body.swap(result);
  };

  rewrite_attr("href", false);
  rewrite_attr("src", true);

  std::string doc;
  doc += "<!DOCTYPE html>\n<html>\n<head>\n";
  doc += "<meta charset=\"utf-8\">\n";
  doc += "<title>" + opts.title + "</title>\n";
  doc += "<link rel=\"stylesheet\" type=\"text/css\" href=\"" + opts.css_href + "\">\n";
  doc += "</head>\n<body>\n";
  doc += body;
  doc += "</body>\n</html>\n";
  return doc;
}

} // namespace roadmaker::helpc
