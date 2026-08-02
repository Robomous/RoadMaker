# RoadMaker documentation site

Astro Starlight project that publishes `docs/user-guide/` as a static site
([ADR-0009](../docs/decisions/0009-documentation-site-tiered-docs.md)).

## The content here is GENERATED — never hand-edit it

`src/content/docs/` and `src/styles/theme.css` are build outputs and are
gitignored. The sources are:

| Output | Source | Generator |
|---|---|---|
| `src/content/docs/**` | `docs/user-guide/**` | `scripts/adapt.mjs` |
| `src/styles/theme.css` | `editor/src/theme/theme.cpp` | `scripts/theme-css.mjs` |

Editing a generated file silently loses the edit on the next build. Change the
source instead.

## The two pipelines share one manifest

`docs/user-guide/index.md` is the ordering manifest for **both** the in-app Qt
Help book and this site: `helpc::build_toc()` reads its links in document order,
and `scripts/adapt.mjs` derives the reference-tier sidebar order from the same
list. That is deliberate — it is what stops the two outputs drifting. A page not
linked from `index.md` is invisible to both.

Tiers ([ADR-0009](../docs/decisions/0009-documentation-site-tiered-docs.md)):

- `reference/` — dual-source: shipped in the `.qhp` **and** on the site.
- `tutorials/`, `guides/` — site only.

## Commands

```sh
npm ci
npm run build            # theme -> adapt -> F1 coverage -> astro build
npm run build:local      # the offline reader that ships in a release
npm run dev              # same as build, then a dev server
npm run licenses         # licence gate over the installed tree
npm test                 # script tests (node:test)
```

The adapter **fails the build** on a broken link, naming the source page and the
target.

## Two builds, one source

| Build | Output | Search | Links |
|---|---|---|---|
| `build` (web) | directory URLs | Pagefind | root-absolute |
| `build:local` | `format: 'file'` | **off** | fully relative |

`build:local` produces the copy bundled in every release, which a reader opens
straight from disk. Three things follow from `file://`, and each is enforced
rather than assumed:

- **`format: 'file'`** — a browser will not serve `index.html` for a bare
  directory over `file://`, so pages are `<slug>.html`.
- **Relative references** — a root-absolute `/…` resolves against the filesystem
  root and 404s. `scripts/relativize.mjs` rewrites them, and
  `scripts/check-local-build.mjs` then verifies the OUTPUT, so the gate still
  fails if the transform were removed or skipped.
- **No search** — Pagefind fetches its index over XHR, which `file://` blocks.
  Switching it off removes the UI too: never ship a search box that does nothing.
  The landing page says where search lives instead.

`relativize.mjs` treats a reference matching no file in the build as an **error**,
not something to rewrite quietly — that is what catches a link to a page that was
renamed. It is idempotent, and `test/relativize.test.mjs` proves that by running
it twice and comparing bytes rather than by asserting it in a comment.

A maintained relative-links integration was considered and rejected: every npm
package here is a permanent obligation under the licence gate, and this transform
is string work over a directory of HTML.

## The reference → guide bridge

A reference page may end with a section under the exact heading `## Full guide`
whose first link points at its tutorial. The **heading** is the marker, so the
authored link stays an ordinary relative Markdown link that renders correctly on
GitHub. Each pipeline then retargets it:

- **this site** — an ordinary site link, via the adapter;
- **the `.qch`** — `rmmanual:<slug>`, which the in-app viewer resolves against the
  packaged manual at runtime and opens in the system browser (ADR-0009 rejects
  embedding a web view). The path is only knowable at runtime, which is why the
  compiler emits a scheme rather than a URL.

Two independent gates keep it honest: `HelpBridge.EveryBridgeTargetIsAPageThatExists`
(C++, over `docs/user-guide`) and the adapter's own broken-link failure. Renaming
a tutorial fails both.

## Licences

Every npm dependency must be MIT/BSD/Apache-2.0-compatible under
[the dependency policy](../docs/standards/dependencies.md). `npm run licenses`
enforces that over the installed tree and runs in CI, so a transitive dependency
cannot introduce a copyleft licence between audits.

Astro's default image service is `sharp`, whose prebuilt libvips binaries are
**LGPL-3.0-or-later** — and Qt is this project's only sanctioned LGPL
dependency. So `astro.config.mjs` uses Astro's passthrough image service, and
`package.json`'s `overrides` points `sharp` at [`stubs/sharp`](stubs/sharp),
a no-op that throws if anything ever imports it. Guide images are editor
screenshots that need no build-time processing.

`--omit=optional` would have been the obvious mechanism and does **not** work:
it also drops rollup's required native binary. Overriding the single package is
the narrowest option npm offers.

## Node

Node LTS is pinned by `.nvmrc` and `engines`. CI uses `npm ci` against the
committed `package-lock.json`.

**No CMake target invokes npm**, and nothing here needs a C++ toolchain: the
theme generator parses `theme.cpp` as text rather than depending on a build
artifact, so a developer build never requires Node and this workflow never
requires a compiler.
