#include "helpc/toc.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace roadmaker::helpc {

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool is_external(const std::string& target) {
  return target.find("://") != std::string::npos || target.rfind("mailto:", 0) == 0;
}

/// Split "page.md#anchor" into ("page.md", "#anchor").
std::pair<std::string, std::string> split_anchor(const std::string& target) {
  const auto hash = target.find('#');
  if (hash == std::string::npos) {
    return {target, {}};
  }
  return {target.substr(0, hash), target.substr(hash)};
}

} // namespace

std::string first_h1(const std::string& markdown) {
  std::istringstream stream(markdown);
  std::string line;
  while (std::getline(stream, line)) {
    // Exactly one '#' followed by a space is an H1; "## " is an H2.
    if (line.size() >= 2 && line[0] == '#' && line[1] == ' ') {
      return trim(line.substr(2));
    }
  }
  return {};
}

std::vector<std::string> extract_links(const std::string& markdown) {
  std::vector<std::string> links;
  for (std::size_t i = 0; i < markdown.size(); ++i) {
    if (markdown[i] != '[') {
      continue;
    }
    // Image links are "![alt](url)"; skip the ones preceded by '!'.
    if (i > 0 && markdown[i - 1] == '!') {
      continue;
    }
    const auto close = markdown.find(']', i);
    if (close == std::string::npos || close + 1 >= markdown.size() || markdown[close + 1] != '(') {
      continue;
    }
    const auto paren = markdown.find(')', close + 2);
    if (paren == std::string::npos) {
      continue;
    }
    links.push_back(trim(markdown.substr(close + 2, paren - (close + 2))));
    i = paren;
  }
  return links;
}

Toc build_toc(const std::filesystem::path& guide_dir) {
  Toc toc;

  const std::string index_md = read_file(guide_dir / "index.md");
  toc.index.rel_path = "index.md";
  toc.index.slug = "index";
  toc.index.title = first_h1(index_md);
  toc.index.tutorial = false;

  std::unordered_set<std::string> seen;
  for (const std::string& raw : extract_links(index_md)) {
    if (raw.empty() || is_external(raw) || raw[0] == '#') {
      continue;
    }
    const auto [path, anchor] = split_anchor(raw);
    if (path.empty() || path.rfind("../", 0) == 0 || path.rfind("./", 0) == 0) {
      continue; // escapes the guide directory
    }
    if (path.size() < 3 || path.substr(path.size() - 3) != ".md") {
      continue;
    }
    if (!std::filesystem::exists(guide_dir / path)) {
      continue;
    }
    if (!seen.insert(path).second) {
      continue; // linked more than once — keep the first
    }
    TocEntry entry;
    entry.rel_path = path;
    entry.slug = path.substr(0, path.size() - 3);
    entry.title = first_h1(read_file(guide_dir / path));
    entry.tutorial = path.rfind("tutorials/", 0) == 0;
    toc.pages.push_back(std::move(entry));
  }
  return toc;
}

std::vector<TocEntry> all_pages(const Toc& toc) {
  std::vector<TocEntry> pages;
  pages.reserve(toc.pages.size() + 1);
  pages.push_back(toc.index);
  pages.insert(pages.end(), toc.pages.begin(), toc.pages.end());
  return pages;
}

} // namespace roadmaker::helpc
