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

  // TOC tree: the index page is the root, its non-tutorial pages are direct
  // children, and tutorial pages nest under a synthetic "Tutorials" node.
  out += "    <toc>\n";
  out += "      <section title=\"" + xml_escape(toc.index.title) + "\" ref=\"index.html\">\n";
  const TocEntry* first_tutorial = nullptr;
  for (const TocEntry& page : toc.pages) {
    if (page.tutorial) {
      if (first_tutorial == nullptr) {
        first_tutorial = &page;
      }
      continue;
    }
    out += section(page, "        ");
  }
  if (first_tutorial != nullptr) {
    out += "        <section title=\"Tutorials\" ref=\"" + xml_escape(first_tutorial->slug) +
           ".html\">\n";
    for (const TocEntry& page : toc.pages) {
      if (page.tutorial) {
        out += section(page, "          ");
      }
    }
    out += "        </section>\n";
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
  // tutorials/ pages render under tutorials/ (their slug keeps the subdir), so
  // both the pages and their img/ assets need their own patterns (#292).
  out += "    <files>\n";
  out += "      <file>*.html</file>\n";
  out += "      <file>help.css</file>\n";
  out += "      <file>img/*.png</file>\n";
  out += "      <file>img/*.gif</file>\n";
  out += "      <file>img/*.jpg</file>\n";
  out += "      <file>tutorials/*.html</file>\n";
  out += "      <file>tutorials/img/*.png</file>\n";
  out += "      <file>tutorials/img/*.gif</file>\n";
  out += "      <file>tutorials/img/*.jpg</file>\n";
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
