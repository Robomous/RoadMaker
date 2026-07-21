# ADR-0009: Documentation site — tiered content, Qt Help + Starlight side by side

- **Status:** ACCEPTED — maintainer approved 2026-07-21
- **Date:** 2026-07-21
- **Deciders:** Armando Anaya

## Context

RoadMaker has exactly one authored documentation source for end users —
`docs/user-guide/` — and exactly one renderer for it: the Qt Help pipeline
built by `help-s1` (#265) and `help-s2` (#266). That pipeline compiles the
Markdown into a `.qch` collection served in-app by a `QTextBrowser`
subclass, with `F1` resolving the active tool or focused panel to its page
and a CI coverage test that fails the build when a registered tool or panel
has no reachable page.

That single renderer is also a ceiling. `QTextBrowser` supports a limited
HTML/CSS subset, so every page — including long, visual, step-by-step
tutorials — has to be written in whatever plain Markdown survives the
`.qhp` path. The cost is already visible: #292 (tutorial images were not
registered in the `.qhp`) and #297 (tutorial parent-relative links rewrite
to guide-root URLs and leave the help system) are both defects of pushing
tutorial-shaped content through a reference-shaped pipeline. Meanwhile
RoadMaker has no web documentation at all: a prospective user cannot read
the guide without cloning the repo or installing the editor, and there is
no versioned manual to point a release at.

The obvious reflexes are both wrong. Replacing Qt Help with a website
throws away instant, offline, context-sensitive `F1` — the single most
valuable documentation affordance an editor has, and the one the coverage
test already protects. Forcing a website's content back through the `.qhp`
renderer condemns tutorials to the lowest common denominator forever and
guarantees more #297-shaped bugs.

## Decision

We keep the Qt Help pipeline permanently and add an Astro Starlight static
site beside it, splitting content by **role**, not by format, so each
pipeline renders only what it is good at.

### 1. Two tiers, one bridge

- **Reference tier → Qt Help (`.qch`, in-app, `F1`).** Short per-tool and
  per-panel pages: what it does, its parameters, its shortcuts, a couple of
  paragraphs. Deliberately plain — a strict CommonMark subset, documented as
  a convention — because `QTextBrowser` renders a limited HTML/CSS subset.
  The `helpId()` mapping and the tool/panel coverage test apply to **this
  tier only**. Reference pages are dual-source: they feed the `.qhp`
  pipeline *and* the site.
- **Guides tier → Starlight only.** Tutorials, workflow guides,
  getting-started, anything long or visual. These pages may use Starlight's
  richer syntax freely, because they never pass through the `.qhp`
  pipeline. Policy for now: plain Markdown plus Starlight asides; MDX and JS
  components are deferred as long as possible so the local `file://` build
  stays simple.
- **The bridge.** Each reference page ends with a standard "full guide"
  link to its matching guide. In the in-app viewer that link opens the
  external browser (`QDesktopServices::openUrl`) into the packaged HTML
  manual; on the site it is an ordinary link. `F1` gives the instant
  answer, one click gives the rich version.

This shrinks the dual-source surface from "everything" to "the reference
pages", which are exactly the pages that were already written plain.

### 2. Single source, restructured by tier

`docs/user-guide/` remains the sole authored source. It gains a tier
structure: `reference/` (dual-source, strict CommonMark) and
`guides/` + `tutorials/` (Starlight-only; the `.qhp` generator ignores
them).

The Starlight project ingests `docs/user-guide/` at build time through an
**adapter script** (Node) that copies content into Starlight's content
directory, synthesizes the required `title` frontmatter from each page's
first H1, rewrites relative links and image references for Starlight
routing, and **fails loudly on a broken link**. Because guides never pass
through `.qhp`, the adapter is a disciplined copier, not a heroic
translator. Its output is generated and gitignored; hand-editing it is
forbidden.

### 3. Tooling and folder

Astro Starlight, in a new top-level `docs-site/` folder in this repo.
`docs/` stays the public contributor source of truth; `docs-site/` is
tooling. Node.js LTS is pinned (exact major via `engines` and `.nvmrc`),
`package-lock.json` is committed, CI uses `npm ci`, and every npm
dependency must be MIT/BSD/Apache-2.0-compatible under the existing
[dependency policy](../standards/dependencies.md). Site CI runs on **Linux
runners only**.

**The C++/CMake build stays completely Node-free.** CMake never invokes
npm. Packaging jobs build the manual with Node first and hand the prebuilt
folder to CMake behind an explicit option (`ROADMAKER_BUNDLE_MANUAL`,
default `OFF`), so a developer build never requires Node.

The site theme reuses the graphite + amber palette so in-app help and the
website read as one product family. Note the as-built constraint: that
palette exists today as C++ constants (`theme::graphite_amber()`), and the
in-app `help.css` is committed **pre-substituted** with flattened hex and
gated byte-for-byte by a test, because `QTextBrowser` does not support CSS
custom properties. There is therefore one source list but no reusable
stylesheet; `docs-s1` owns defining a single export path from the theme to
site CSS custom properties, so the palette is never transcribed by hand
into a second place.

### 4. Two site build modes

- **Local reader build**, shipped inside each release: opens directly from
  `file://` in a browser with no server. That requires
  `build.format: 'file'` plus fully relative asset and link references
  (either a maintained relative-links integration or an idempotent
  post-processing step — `docs-s2` evaluates and justifies the choice).
  Pagefind search does not work over `file://` in mainstream browsers, so
  search is **disabled** in this variant with a note on the local landing
  page: we never ship a broken search box. CI fails the build if any
  root-absolute path survives.
- **Web build**: standard Starlight output with search enabled, built with
  a configurable `--base=/<segment>/`.

Install layout for the local manual: `RoadMaker.app/Contents/Resources/manual/`
on macOS, `share/roadmaker/manual/` on Linux, `manual/` beside the
executable on Windows. A Help-menu action opens `manual/index.html` in the
default browser; when the folder is absent (a dev build) it shows a
friendly pointer to the web docs rather than failing silently. `F1`
behavior is untouched.

### 5. Versioned web publishing

GitHub Actions assembles a published tree: `dev/` from `main`, `vX.Y.Z/`
from each release tag the maintainer pushes, `latest/` as a copy of the
highest semver, a root redirect to `latest/` (or `dev/` while no release
exists), and a root `versions.json` that a small header dropdown reads to
switch versions, preserving the current page path when it exists in the
target version.

The assembled tree lives on a publishing branch (`docs-published`) that
**AWS Amplify Hosting serves prebuilt**. Amplify does not build from
source: tag-driven multi-version assembly is Actions territory, and a
prebuilt branch keeps the hosting layer dumb and auditable. Workflows only
*react* to tags — they never create them, consistent with the roadmap rule
that only the maintainer publishes. All Amplify console setup and every
publishing action are the maintainer's; the implementation sprints deliver
`amplify.yml`, the workflows, and a runbook. The pipeline must work with
`dev/` alone today (no tags exist after the roadmap reset) and grow
versions when v0.1.0 lands.

## Alternatives rejected

- **One unified pipeline feeding both renderers.** Every page would have to
  satisfy `QTextBrowser` *and* Starlight, so the whole corpus inherits the
  `.qhp` renderer's ceiling — the exact constraint that produced #292 and
  #297. Rejected: it makes the weakest renderer the project's authoring
  standard.
- **Replace Qt Help entirely with the website.** Loses offline,
  instant, context-sensitive `F1` and the tool/panel coverage gate that
  keeps documentation from rotting as tools are added. Rejected: `F1` is
  the highest-value documentation feature the editor has.
- **Embed `QWebEngineView` to render the site in-app.** Pulls Chromium into
  an editor whose entire dependency policy is built on staying small and
  permissively licensed, to solve a problem the operating system's default
  browser solves for free. Rejected outright.
- **Let Amplify build the site from source.** Amplify builds one branch at
  a time; assembling `dev/` plus every `vX.Y.Z/` plus `latest/` plus
  `versions.json` is multi-ref work that belongs in Actions. Building in
  Amplify would also put Node build logic in a console UI instead of in the
  repo.
- **A Starlight versioning plugin.** Third-party version plugins own the
  version tree inside the site build; here the tree spans refs (`main` plus
  tags) and must be reproducible from Actions alone. A plain assembly step
  plus `versions.json` is less magic, has no extra dependency, and degrades
  to a single `dev/` directory on day one.
- **MkDocs** (named as a possible future generator in the docs-tree
  conventions). Starlight is chosen for first-class `file://`-capable static
  output, built-in versionable routing, and a theme system that can consume
  the editor's existing tokens; the conventions page is updated in
  `docs-s4`.

## Consequences

- Authors gain a rule instead of a judgment call: reference pages stay
  plain and get `F1`; guides get rich syntax and the website. `docs-s4`
  ships the authoring guide that states it.
- The `.qhp` generator's input narrows to `docs/user-guide/reference/`.
  `docs-s1` must move the files and keep the coverage test green through
  the move — the riskiest single step in the workstream, because the
  generator's file list is not a glob: `docs/user-guide/index.md` **is** the
  manifest, page slugs are repo-relative paths, and the tool/panel gate
  asserts both `<slug>.md` on disk and the literal `(<slug>.md)` inside
  `index.md`. Moving a page changes its slug, its `qthelp://` URL, and its
  keyword id in lockstep.
- **Interaction with #297.** The tiered model *reduces the blast radius* of
  #297 by removing tutorials from the `.qhp` path, but it does not fix it:
  reference pages can still link to each other relatively, and the link
  rewriter is still wrong. #297 stays an independent P2 bug with its own
  fix and its own regression test; no sprint in this workstream closes it.
- Two build systems live in one repo. The mitigation is strict separation:
  CMake never calls npm, the site's CI is a separate Linux-only workflow
  triggered by `docs/user-guide/**` and `docs-site/**` paths, and
  dependency hygiene for `docs-site/` is scoped so it adds zero noise to
  the C++ side.
- The IP rule applies unchanged to all authored content, issues, commits,
  and site copy: no third-party product names, RoadMaker vocabulary and
  ASAM concepts only. Naming build dependencies (Astro, Starlight, Node,
  Pagefind) in tooling and configuration is fine, as it is for Eigen or
  Manifold.
- **Reversal cost.** Low for the site (delete `docs-site/`, the publishing
  branch, and one workflow; the authored Markdown is untouched). Moderate
  for the tier restructure, which is a `git mv` plus a generator-input
  change and would have to be undone the same way.

## References

- [Roadmap — Documentation site](../roadmap/README.md#documentation-site)
  and its release-gate extension
- [ADR-0007](0007-roadmap-reset-road-to-parity.md) — single-release model;
  only the maintainer tags and publishes
- [Dependencies & licensing](../standards/dependencies.md) ·
  [Product parity & IP](../standards/product-parity.md)
- #265 `help-s1` (Qt Help pipeline + in-app viewer) · #266 `help-s2` (F1
  context help, coverage test, seed tutorials) · #292 (tutorial images in
  the `.qhp`, fixed) · #297 (tutorial relative links, open)
