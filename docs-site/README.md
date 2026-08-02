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
npm run dev              # same, then a dev server
npm run licenses         # licence gate over the installed tree
```

The adapter **fails the build** on a broken link, naming the source page and the
target.

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
