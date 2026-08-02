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

#include "helpc/qhp.hpp"

namespace roadmaker::helpc {

std::string xml_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
    }
  }
  return out;
}

namespace {

std::string section(const TocEntry& entry, const std::string& indent) {
  return indent + "<section title=\"" + xml_escape(entry.title) + "\" ref=\"" +
         xml_escape(entry.slug) + ".html\"/>\n";
}

} // namespace

std::string build_qhp(const Toc& toc, const QhpOptions& opts) {
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<QtHelpProject version=\"1.0\">\n";
  out += "  <namespace>" + xml_escape(opts.namespace_) + "</namespace>\n";
  out += "  <virtualFolder>" + xml_escape(opts.folder) + "</virtualFolder>\n";
  out += "  <filterSection>\n";

  // TOC tree: the index page is the root and every reference page is a direct
  // child. The synthetic "Tutorials" node is gone with docs-s1 — tutorials are
  // Starlight-only now (ADR-0009), so build_toc never yields one.
  out += "    <toc>\n";
  out += "      <section title=\"" + xml_escape(toc.index.title) + "\" ref=\"index.html\">\n";
  for (const TocEntry& page : toc.pages) {
    out += section(page, "        ");
  }
  out += "      </section>\n";
  out += "    </toc>\n";

  // Exactly one keyword per page; the id is the slug (stable help-s2 contract).
  out += "    <keywords>\n";
  for (const TocEntry& page : all_pages(toc)) {
    out += "      <keyword name=\"" + xml_escape(page.title) + "\" id=\"" + xml_escape(page.slug) +
           "\" ref=\"" + xml_escape(page.slug) + ".html\"/>\n";
  }
  out += "    </keywords>\n";

  // qhelpgenerator expands <file> wildcards per directory, never recursively:
  // reference/ pages render under reference/ (their slug keeps the subdir), so
  // both the pages and their img/ assets need their own patterns (#292 — the
  // bug this shape exists to prevent, and the reason a NEW tier folder must
  // never be added without adding its patterns here).
  out += "    <files>\n";
  out += "      <file>*.html</file>\n";
  out += "      <file>help.css</file>\n";
  out += "      <file>img/*.png</file>\n";
  out += "      <file>img/*.gif</file>\n";
  out += "      <file>img/*.jpg</file>\n";
  out += "      <file>reference/*.html</file>\n";
  out += "      <file>reference/img/*.png</file>\n";
  out += "      <file>reference/img/*.gif</file>\n";
  out += "      <file>reference/img/*.jpg</file>\n";
  out += "    </files>\n";

  out += "  </filterSection>\n";
  out += "</QtHelpProject>\n";
  return out;
}

std::string build_qhcp(const QhpOptions& opts) {
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<QHelpCollectionProject version=\"1.0\">\n";
  out += "  <docFiles>\n";
  out += "    <register>\n";
  out += "      <file>" + xml_escape(opts.qch_file) + "</file>\n";
  out += "    </register>\n";
  out += "  </docFiles>\n";
  out += "</QHelpCollectionProject>\n";
  return out;
}

} // namespace roadmaker::helpc
