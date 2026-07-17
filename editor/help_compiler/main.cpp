// rm_helpc — compiles the committed Markdown user guide into the inputs
// qhelpgenerator needs (rendered HTML + .qhp + .qhcp). Build-time host tool;
// Qt-free. See editor/CMakeLists.txt for how the outputs become the shipped
// .qch/.qhc.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "helpc/qhp.hpp"
#include "helpc/render.hpp"
#include "helpc/toc.hpp"

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void write_file(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << content;
}

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "rm_helpc: " << message << '\n';
  std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
  std::unordered_map<std::string, std::string> args;
  for (int i = 1; i + 1 < argc; i += 2) {
    args[argv[i]] = argv[i + 1];
  }

  const auto require = [&](const std::string& flag) -> std::string {
    const auto it = args.find(flag);
    if (it == args.end()) {
      fail("missing required flag " + flag);
    }
    return it->second;
  };

  const fs::path guide = require("--guide");
  const fs::path css = require("--css");
  const fs::path out = require("--out");

  roadmaker::helpc::QhpOptions qhp;
  if (args.count("--namespace") != 0U) {
    qhp.namespace_ = args["--namespace"];
  }
  if (args.count("--folder") != 0U) {
    qhp.folder = args["--folder"];
  }
  if (args.count("--version") != 0U) {
    qhp.version = args["--version"];
  }

  if (!fs::exists(guide / "index.md")) {
    fail("no index.md in guide dir " + guide.string());
  }

  const fs::path html_dir = out / "html";
  const fs::path img_dir = html_dir / "img";
  fs::create_directories(img_dir);

  // Stylesheet.
  fs::copy_file(css, html_dir / "help.css", fs::copy_options::overwrite_existing);

  // The guide's own img/ directory ships wholesale.
  const fs::path guide_img = guide / "img";
  if (fs::exists(guide_img)) {
    for (const auto& entry : fs::directory_iterator(guide_img)) {
      if (entry.is_regular_file()) {
        fs::copy_file(
            entry.path(), img_dir / entry.path().filename(), fs::copy_options::overwrite_existing);
      }
    }
  }

  const roadmaker::helpc::Toc toc = roadmaker::helpc::build_toc(guide);

  for (const roadmaker::helpc::TocEntry& page : roadmaker::helpc::all_pages(toc)) {
    roadmaker::helpc::RenderOptions opts;
    opts.title = page.title;
    opts.guide_dir = guide;
    opts.img_out_dir = img_dir;
    const std::string html = roadmaker::helpc::render_page(read_file(guide / page.rel_path), opts);
    write_file(html_dir / (page.slug + ".html"), html);
  }

  // The .qhp sits with the HTML so its <file>/ref paths resolve; the .qhcp sits
  // in `out` beside the .qch qhelpgenerator produces from the .qhp.
  write_file(html_dir / "roadmaker.qhp", roadmaker::helpc::build_qhp(toc, qhp));
  write_file(out / "roadmaker.qhcp", roadmaker::helpc::build_qhcp(qhp));

  std::cout << "rm_helpc: wrote " << (toc.pages.size() + 1) << " pages to " << html_dir.string()
            << '\n';
  return 0;
}
