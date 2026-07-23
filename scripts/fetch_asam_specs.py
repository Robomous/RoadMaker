#!/usr/bin/env python3

# Copyright 2026 Robomous
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Fetch ASAM specifications as local Markdown references for RoadMaker.

Downloads the ASAM OpenDRIVE and ASAM OpenSCENARIO XML specification HTML
from publications.pages.asam.net, converts it to Markdown (one file per
top-level chapter), and writes navigable INDEX.md files into
.claude/references/asam/.

Usage:
    python scripts/fetch_asam_specs.py --std all
    python scripts/fetch_asam_specs.py --std opendrive --out .claude/references/asam

Requirements (disposable venv — NOT project dependencies):
    python3.11+  with  requests, beautifulsoup4, markdownify

Policy: the fetched spec bodies are gitignored (OpenSCENARIO redistribution
is not clearly permitted); only this script and the INDEX files are meant
to be shared. Outputs are build artifacts — fix this script, never
hand-edit the generated .md files.
"""

from __future__ import annotations

import argparse
import datetime
import re
import shutil
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import urljoin, urlparse

import requests
from bs4 import BeautifulSoup
from markdownify import markdownify

BASE = "https://publications.pages.asam.net/standards"
USER_AGENT = "RoadMaker-spec-fetch (github.com/robomous/roadmaker)"
REQUEST_DELAY_S = 0.5
MAX_FILE_BYTES = 250 * 1024
# Packing budget: leave headroom for the file header/title, whose exact size
# is only known after rendering — keeps every output strictly under the cap.
PACK_BUDGET_BYTES = MAX_FILE_BYTES - 4096
TOTAL_SIZE_GUARD_BYTES = 40 * 1024 * 1024

# Entry points. "latest" is resolved to its concrete version at run time and
# the concrete version is what lands in directory names and INDEX files.
SPEC_ENTRIES: dict[str, list[dict]] = {
    "opendrive": [
        {
            "name": "ASAM OpenDRIVE",
            "slug": "opendrive",
            "entry": f"{BASE}/ASAM_OpenDRIVE/ASAM_OpenDRIVE_Specification/latest/specification/index.html",
        },
        {
            "name": "ASAM OpenDRIVE",
            "slug": "opendrive",
            "entry": f"{BASE}/ASAM_OpenDRIVE/ASAM_OpenDRIVE_Specification/v1.8.1/specification/index.html",
        },
    ],
    "openscenario": [
        {
            "name": "ASAM OpenSCENARIO XML",
            "slug": "openscenario-xml",
            "entry": f"{BASE}/ASAM_OpenSCENARIO/ASAM_OpenSCENARIO_XML/latest/index.html",
        },
    ],
}

# Topic -> chapter-directory keyword lookup, tuned to RoadMaker work.
OPENDRIVE_TOPICS: list[tuple[str, list[str]]] = [
    ("Reference line geometry / clothoids & spirals", ["geometr"]),
    ("Lane sections & widths", ["lane"]),
    ("Elevation & superelevation", ["road", "elevation", "lateral"]),
    ("Junctions & connecting roads", ["junction"]),
    ("Signals", ["signal"]),
    ("Objects", ["object"]),
    ("Coordinate systems", ["coordinate"]),
    ("File header & versioning", ["architecture", "general"]),
]

OPENSCENARIO_TOPICS: list[tuple[str, list[str]]] = [
    ("General concepts (parameters, catalogs, coordinates)", ["general_concepts"]),
    ("Scenario structure (Storyboard, ManeuverGroup)", ["components_scenario"]),
    ("Runtime behavior (Triggers, conditions, actions)", ["runtime_scenario"]),
    ("Reuse mechanisms (Catalog, ParameterDeclaration)", ["reuse"]),
    ("Scenario creation guidelines", ["scenario_creation"]),
    ("Examples", ["example"]),
    ("Element/class reference (LanePosition, Trigger, ...)", ["classes"]),
]

RULE_ID_RE = re.compile(r"asam\.net:[a-z]+:[0-9][0-9.]*:[A-Za-z0-9_.]+")
META_REFRESH_RE = re.compile(
    r'http-equiv="refresh"[^>]*url=([^">]+)"', re.IGNORECASE
)
VERSION_RE = re.compile(r"/v(\d+\.\d+\.\d+)/")

session = requests.Session()
session.headers["User-Agent"] = USER_AGENT


def log(msg: str) -> None:
    print(msg, flush=True)


def fetch(url: str) -> str:
    """Polite GET: fixed delay, 3 retries with backoff on 5xx, hard fail."""
    last_error: Exception | None = None
    for attempt in range(3):
        time.sleep(REQUEST_DELAY_S if attempt == 0 else 2.0 * (attempt + 1))
        try:
            resp = session.get(url, timeout=60)
            if resp.status_code >= 500:
                last_error = RuntimeError(f"HTTP {resp.status_code} for {url}")
                continue
            resp.raise_for_status()
            return resp.text
        except requests.RequestException as exc:  # noqa: PERF203
            last_error = exc
    raise RuntimeError(f"failed to fetch {url} after 3 attempts: {last_error}")


def resolve_entry(entry_url: str, notes: list[str]) -> tuple[str, str]:
    """Follow meta-refresh redirects; return (concrete index URL, version)."""
    url = entry_url
    for _ in range(4):
        html = fetch(url)
        m = META_REFRESH_RE.search(html)
        if not m:
            break
        target = urljoin(url, m.group(1).strip())
        if target != url:
            notes.append(f"`{url}` redirected to `{target}`")
        url = target
    m = VERSION_RE.search(url)
    if not m:
        raise RuntimeError(f"could not determine concrete version from {url}")
    return url, m.group(1)


def discover_pages(index_url: str, index_html: str) -> list[str]:
    """Collect page URLs from the spec's own nav, in nav order.

    Restricted strictly to the resolved version's path prefix; never
    brute-forces paths or crawls outside it.
    """
    prefix = index_url.rsplit("/", 1)[0] + "/"
    soup = BeautifulSoup(index_html, "html.parser")
    nav = soup.select_one("nav.nav-menu")
    if nav is None:
        raise RuntimeError(f"no nav.nav-menu found in {index_url}")
    pages: list[str] = []
    seen: set[str] = set()
    for a in nav.find_all("a", href=True):
        url = urljoin(index_url, a["href"]).split("#", 1)[0].split("?", 1)[0]
        if not url.startswith(prefix) or not url.endswith(".html"):
            continue
        if url == index_url or url in seen:
            continue
        seen.add(url)
        pages.append(url)
    return pages


def table_is_complex(table) -> bool:
    if table.find("table") is not None:
        return True
    for cell in table.find_all(["td", "th"]):
        if cell.get("rowspan", "1") != "1" or cell.get("colspan", "1") != "1":
            return True
    return False


def page_to_markdown(page_html: str, page_url: str) -> tuple[str, str]:
    """Convert one spec page to Markdown. Returns (markdown, page title)."""
    soup = BeautifulSoup(page_html, "html.parser")
    article = soup.select_one("article.doc") or soup.select_one(".doc") or soup.body
    if article is None:
        raise RuntimeError(f"no content container in {page_url}")
    for selector in (
        "nav", "aside", "script", "style", "footer",
        ".pagination", ".breadcrumbs", ".page-versions", ".toolbar", ".banner",
    ):
        for el in article.select(selector):
            el.decompose()

    title_el = article.find(["h1", "h2"])
    title = title_el.get_text(" ", strip=True) if title_el else Path(urlparse(page_url).path).stem

    # Figures: keep a link to the original image, never download binaries.
    for img in article.find_all("img"):
        alt = img.get("alt") or img.get("title") or Path(urlparse(img.get("src", "")).path).name
        img.replace_with(f"[Figure: {alt}]({urljoin(page_url, img.get('src', ''))})")

    # Tables that Markdown cannot represent stay as raw HTML (correctness
    # over prettiness).
    raw_tables: dict[str, str] = {}
    for i, table in enumerate(article.find_all("table")):
        if table_is_complex(table):
            token = f"RMRAWTABLE{i}TOKEN"
            raw_tables[token] = str(table)
            table.replace_with(token)

    md = markdownify(str(article), heading_style="ATX", bullets="-")
    for token, html_src in raw_tables.items():
        md = md.replace(token, f"\n\n{html_src}\n\n")

    # Demote headings one level: chapter file owns '#', pages start at '##'.
    md = re.sub(r"^(#{1,5}) ", r"#\1 ", md, flags=re.MULTILINE)
    md = re.sub(r"\n{3,}", "\n\n", md).strip()
    return md, title


@dataclass
class Section:
    url: str
    title: str
    markdown: str

    def render(self) -> str:
        return f"<!-- source: {self.url} -->\n\n{self.markdown}\n"


@dataclass
class ChapterFile:
    filename: str
    title: str
    first_url: str
    size_bytes: int = 0
    section_count: int = 0
    split_note: str = ""


@dataclass
class SpecResult:
    name: str
    dirname: str
    version: str
    index_url: str
    pages_discovered: int = 0
    pages_fetched: int = 0
    chapters: list[ChapterFile] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    total_bytes: int = 0


def file_header(spec_name: str, version: str, source_url: str) -> str:
    today = datetime.date.today().isoformat()
    return (
        f"<!--\nsource: {source_url}\nspec: {spec_name}\nversion: {version}\n"
        f"fetched: {today}\n"
        "© ASAM e.V. — fetched for local reference use in RoadMaker development.\n"
        "Generated by scripts/fetch_asam_specs.py — do not hand-edit.\n-->\n\n"
    )


def pretty_chapter_title(chapter_key: str) -> str:
    words = re.sub(r"^\d+_?", "", chapter_key).replace("_", " ").strip()
    return words.capitalize() if words else chapter_key


def write_chapter(
    out_dir: Path,
    spec: SpecResult,
    chapter_key: str,
    sections: list[Section],
) -> None:
    """Write one chapter file, splitting at ~250 KB on section boundaries."""
    title = pretty_chapter_title(chapter_key)
    header = file_header(spec.name, spec.version, sections[0].url)
    parts: list[list[Section]] = [[]]
    size = len(header) + len(title) + 4
    for sec in sections:
        rendered_len = len(sec.render().encode()) + 2
        if parts[-1] and size + rendered_len > PACK_BUDGET_BYTES:
            parts.append([])
            size = len(header)
        parts[-1].append(sec)
        size += rendered_len

    for i, part in enumerate(parts):
        suffix = "" if i == 0 else f"_part{i + 1}"
        filename = f"{chapter_key}{suffix}.md"
        body = header + f"# {title}" + (f" (part {i + 1})" if i else "") + "\n\n"
        body += "\n\n".join(sec.render() for sec in part)
        path = out_dir / filename
        path.write_text(body, encoding="utf-8")
        n = path.stat().st_size
        spec.total_bytes += n
        spec.chapters.append(
            ChapterFile(
                filename=filename,
                title=title + (f" (part {i + 1}/{len(parts)})" if len(parts) > 1 else ""),
                first_url=part[0].url,
                size_bytes=n,
                section_count=len(part),
                split_note=f"split into {len(parts)} parts (>{MAX_FILE_BYTES // 1024} KB)" if len(parts) > 1 else "",
            )
        )


def write_class_groups(out_dir: Path, spec: SpecResult, sections: list[Section]) -> None:
    """Group the generated per-class reference alphabetically into ≤250 KB files."""
    classes_dir = out_dir / "classes"
    classes_dir.mkdir(parents=True, exist_ok=True)
    sections = sorted(sections, key=lambda s: s.title.lower())
    header_probe = file_header(spec.name, spec.version, "grouped")

    groups: list[list[Section]] = [[]]
    size = len(header_probe) + 64
    for sec in sections:
        rendered_len = len(sec.render().encode()) + 2
        if groups[-1] and size + rendered_len > PACK_BUDGET_BYTES:
            groups.append([])
            size = len(header_probe) + 64
        groups[-1].append(sec)
        size += rendered_len

    used_names: set[str] = set()
    for group in groups:
        first, last = group[0].title[0].lower(), group[-1].title[0].lower()
        stem = f"classes_{first}-{last}"
        candidate, n = stem, 2
        while candidate in used_names:
            candidate = f"{stem}_{n}"
            n += 1
        used_names.add(candidate)
        filename = f"{candidate}.md"
        title = f"Generated class reference: {group[0].title} … {group[-1].title}"
        body = file_header(spec.name, spec.version, group[0].url) + f"# {title}\n\n"
        body += "\n\n".join(sec.render() for sec in group)
        path = classes_dir / filename
        path.write_text(body, encoding="utf-8")
        nbytes = path.stat().st_size
        spec.total_bytes += nbytes
        spec.chapters.append(
            ChapterFile(
                filename=f"classes/{filename}",
                title=title,
                first_url=group[0].url,
                size_bytes=nbytes,
                section_count=len(group),
            )
        )


def collect_rule_ids(out_dir: Path) -> dict[str, list[str]]:
    """Map normative rule ID -> files containing it (IDs are load-bearing)."""
    found: dict[str, list[str]] = {}
    for path in sorted(out_dir.rglob("*.md")):
        if path.name == "INDEX.md":
            continue
        text = path.read_text(encoding="utf-8")
        rel = str(path.relative_to(out_dir))
        for rule_id in set(RULE_ID_RE.findall(text)):
            found.setdefault(rule_id, []).append(rel)
    return found


def topic_table(topics: list[tuple[str, list[str]]], chapters: list[ChapterFile]) -> str:
    lines = ["| Topic | File(s) |", "|---|---|"]
    for topic, keywords in topics:
        hits = [
            c.filename
            for c in chapters
            if any(k in c.filename.lower() for k in keywords)
        ]
        # Collapse split parts and class groups to keep the table readable.
        if any(h.startswith("classes/") for h in hits):
            hits = ["classes/ (alphabetical groups)"]
        lines.append(f"| {topic} | {', '.join(f'`{h}`' for h in hits) or '—'} |")
    return "\n".join(lines)


def write_spec_index(out_dir: Path, spec: SpecResult, is_openscenario: bool) -> None:
    today = datetime.date.today().isoformat()
    rule_ids = collect_rule_ids(out_dir)
    lines = [
        f"# {spec.name} {spec.version} — local reference index",
        "",
        "Generated by `scripts/fetch_asam_specs.py` — do not hand-edit.",
        "© ASAM e.V. — fetched for local reference use in RoadMaker development.",
        "",
        "## Fetch metadata",
        "",
        f"- Source: {spec.index_url}",
        f"- Version: {spec.version} (concrete; never \"latest\")",
        f"- Fetched: {today}",
        f"- Pages discovered in nav: {spec.pages_discovered}",
        f"- Pages fetched: {spec.pages_fetched}",
        f"- Files written: {len(spec.chapters)}",
        f"- Total size: {spec.total_bytes / 1024:.0f} KB",
    ]
    if spec.notes:
        lines += ["", "## Fetch notes", ""] + [f"- {n}" for n in spec.notes]

    lines += [
        "",
        "## Topic → file lookup",
        "",
        topic_table(OPENSCENARIO_TOPICS if is_openscenario else OPENDRIVE_TOPICS, spec.chapters),
        "",
        "## Chapters",
        "",
        "| File | Chapter | Sections | Size | Source |",
        "|---|---|---|---|---|",
    ]
    for c in spec.chapters:
        note = f" — {c.split_note}" if c.split_note else ""
        lines.append(
            f"| `{c.filename}` | {c.title}{note} | {c.section_count} "
            f"| {c.size_bytes / 1024:.0f} KB | {c.first_url} |"
        )

    if is_openscenario:
        xsd = out_dir / "schema" / "OpenSCENARIO.xsd"
        status = "PRESENT" if xsd.exists() else "MISSING"
        lines += [
            "",
            "## Normative XSD schema (manual step)",
            "",
            "The XSD schema is the normative core of OpenSCENARIO XML and is",
            "distributed only in ASAM's gated download bundle. Maintainer action:",
            "download the standard bundle from",
            "https://www.asam.net/standards/detail/openscenario-xml/ and place",
            f"`OpenSCENARIO.xsd` into `{out_dir.name}/schema/`.",
            "",
            f"- Current status: **{status}** (`schema/OpenSCENARIO.xsd`)",
        ]

    lines += ["", "## Normative rule IDs", ""]
    if rule_ids:
        lines += [
            f"{len(rule_ids)} unique rule IDs found in this spec's text.",
            "",
            "| Rule ID | File(s) |",
            "|---|---|",
        ]
        for rule_id in sorted(rule_ids):
            files = ", ".join(f"`{f}`" for f in sorted(set(rule_ids[rule_id])))
            lines.append(f"| `{rule_id}` | {files} |")
    else:
        lines.append("No `asam.net:...` rule IDs found in this spec's text.")

    (out_dir / "INDEX.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def crawl_spec(spec_def: dict, out_root: Path) -> SpecResult:
    notes: list[str] = []
    index_url, version = resolve_entry(spec_def["entry"], notes)
    dirname = f"{spec_def['slug']}-{version}"
    log(f"==> {spec_def['name']} {version}  ({index_url})")

    out_dir = out_root / dirname
    if out_dir.exists():  # idempotent: clean re-fetch
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    index_html = fetch(index_url)
    pages = discover_pages(index_url, index_html)
    spec = SpecResult(
        name=spec_def["name"], dirname=dirname, version=version,
        index_url=index_url, pages_discovered=len(pages), notes=notes,
    )

    prefix = index_url.rsplit("/", 1)[0] + "/"
    chapters: dict[str, list[Section]] = {}
    class_sections: list[Section] = []
    order: list[str] = []
    for n, url in enumerate(pages, 1):
        rel = url[len(prefix):]
        md, title = page_to_markdown(fetch(url), url)
        spec.pages_fetched += 1
        if n % 25 == 0 or n == len(pages):
            log(f"    {n}/{len(pages)} pages")
        if not md.strip():
            raise RuntimeError(f"empty content extracted from {url}")
        section = Section(url=url, title=title, markdown=md)
        if rel.startswith("generated/content/"):
            class_sections.append(section)
            continue
        key = rel.split("/", 1)[0].removesuffix(".html")
        if key not in chapters:
            chapters[key] = []
            order.append(key)
        chapters[key].append(section)

    for key in order:
        write_chapter(out_dir, spec, key, chapters[key])
    if class_sections:
        write_class_groups(out_dir, spec, class_sections)

    write_spec_index(out_dir, spec, is_openscenario=spec_def["slug"].startswith("openscenario"))
    log(f"    wrote {len(spec.chapters)} files, {spec.total_bytes / 1024:.0f} KB -> {out_dir}")
    return spec


def write_master_index(out_root: Path) -> None:
    """Rebuild the master index from what is on disk, so partial re-runs
    (--std opendrive only, etc.) never drop the other specs' rows."""
    rows = []
    for spec_dir in sorted(p for p in out_root.iterdir() if (p / "INDEX.md").is_file()):
        idx = (spec_dir / "INDEX.md").read_text(encoding="utf-8")
        title = re.search(r"^# (.+?) (\d+\.\d+\.\d+) — local reference index", idx, re.M)
        fetched = re.search(r"^- Fetched: (\S+)", idx, re.M)
        pages = re.search(r"^- Pages fetched: (\d+)", idx, re.M)
        size = re.search(r"^- Total size: (\d+) KB", idx, re.M)
        if not (title and fetched and pages and size):
            raise RuntimeError(f"unparseable INDEX.md in {spec_dir}")
        rows.append(
            f"| `{spec_dir.name}/` | {title.group(1)} | {title.group(2)} "
            f"| {pages.group(1)} | {size.group(1)} KB | {fetched.group(1)} |"
        )
    lines = [
        "# ASAM specification references — master index",
        "",
        "Local text copies of the ASAM standards RoadMaker implements.",
        "Generated by `scripts/fetch_asam_specs.py` — see policy below.",
        "",
        "## Contents",
        "",
        "| Directory | Spec | Version | Pages | Size | Fetched |",
        "|---|---|---|---|---|---|",
        *rows,
    ]
    lines += [
        "",
        "Start at each directory's `INDEX.md` (chapter map + topic→file lookup).",
        "",
        "## Regenerating",
        "",
        "```sh",
        "python -m venv /tmp/asam-fetch-venv",
        "/tmp/asam-fetch-venv/bin/pip install requests beautifulsoup4 markdownify",
        "/tmp/asam-fetch-venv/bin/python scripts/fetch_asam_specs.py --std all",
        "```",
        "",
        "## Licensing policy",
        "",
        "- ASAM OpenDRIVE: ASAM grants everyone a basic, non-exclusive and",
        "  unlimited license to use the standard — storing it locally is fine.",
        "- ASAM OpenSCENARIO XML: use-permitted, redistribution-unclear.",
        "- Therefore: the fetch script and INDEX files are committed; the",
        "  fetched spec bodies are gitignored and regenerated locally. Do NOT",
        "  commit spec bodies without maintainer approval.",
        "",
        "## Rules",
        "",
        "- These files are read-only artifacts: fix `scripts/fetch_asam_specs.py`,",
        "  never hand-edit the outputs.",
        "- Directory names always carry the concrete version, never \"latest\".",
        "",
        "## Out of scope (future milestones)",
        "",
        "- ASAM OpenSCENARIO DSL 2.x, ASAM OpenCRG, and other ASAM standards",
        "  are intentionally not fetched yet; add them when their milestones",
        "  arrive.",
    ]
    (out_root / "INDEX.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--std", choices=["opendrive", "openscenario", "all"], default="all")
    parser.add_argument("--out", type=Path, default=Path(".claude/references/asam"))
    args = parser.parse_args()

    specs: list[dict] = []
    if args.std in ("opendrive", "all"):
        specs += SPEC_ENTRIES["opendrive"]
    if args.std in ("openscenario", "all"):
        specs += SPEC_ENTRIES["openscenario"]

    args.out.mkdir(parents=True, exist_ok=True)
    results = [crawl_spec(spec_def, args.out) for spec_def in specs]

    total = sum(r.total_bytes for r in results)
    if total > TOTAL_SIZE_GUARD_BYTES:
        log(f"WARNING: total fetched size {total / 1024 / 1024:.1f} MB exceeds "
            "40 MB guard — check for over-crawling before trusting this output.")

    write_master_index(args.out)
    for r in results:
        log(f"{r.name} {r.version}: {r.pages_fetched}/{r.pages_discovered} pages, "
            f"{len(r.chapters)} files, {r.total_bytes / 1024:.0f} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
