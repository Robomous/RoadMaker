# Writing user documentation

*I want to document a thing — where does it go, and what may I write?*

This page answers both, concretely enough to follow without reading any
generator source. Decided in
[ADR-0009](../decisions/0009-documentation-site-tiered-docs.md).

It covers `docs/user-guide/` — the documentation **users** read. Contributor
documentation (everything else under `docs/`) has no pipeline and no syntax
budget; write it as you like.

## The one rule everything else follows from

`docs/user-guide/` is the **only** place this content is authored. Two
generators read it:

| Generator | Produces | Reads |
|---|---|---|
| `rm_helpc` (C++) | the in-app `.qch` book that `F1` opens | `index.md` + `reference/` |
| `docs-site/scripts/adapt.mjs` (Node) | the web site and the offline manual | everything |

**Never hand-edit generated output.** `docs-site/src/content/docs/` and
`docs-site/src/styles/theme.css` are build products and are gitignored; an edit
there is silently lost on the next build. Change the source.

## Which tier?

Ask what the page is *for*, not how long it is.

| | **Reference** | **Guides** |
|---|---|---|
| Where | `docs/user-guide/reference/` | `docs/user-guide/tutorials/` (and `guides/`) |
| What | one tool or one panel: what it does, its parameters, its shortcuts | a walkthrough that strings tools together to build something |
| Rendered by | the in-app book **and** the site | the site only |
| Syntax | strict CommonMark subset — see below | Markdown plus Starlight asides |
| Reached by | `F1` in the editor, and the site | the site |

A reference page is short and answers "what is this control?" while the user is
looking at it. A guide answers "how do I build a T-junction?" and may be long
and heavily illustrated.

If a page is genuinely both — a reference head with a tutorial welded on — split
it. `reference/objects-signals.md` is the standing example of one that has not
been split yet.

### Adding a reference page

1. Write `docs/user-guide/reference/<slug>.md`, starting with a single `# H1`.
2. **Link it from `docs/user-guide/index.md`.** This is the step that is easy to
   forget and silent to skip — see the manifest rule below.
3. If the page documents a tool or a panel that `F1` should open it for, add the
   mapping in `editor/src/help/help_registry.cpp`. `test_help_registry.cpp`
   gates that every tool has one.

### Adding a guide page

1. Write `docs/user-guide/tutorials/<slug>.md`, starting with a single `# H1`.
2. Link it from `docs/user-guide/index.md` so readers can find it. The `.qhp`
   generator skips the `tutorials/` prefix deliberately, so this link orders the
   page on the site without pulling it into the in-app book.

## `index.md` is the ordering manifest, for both pipelines

`docs/user-guide/index.md` is not a courtesy table of contents. **Both**
generators read its links, in document order:

- `helpc::build_toc()` ingests exactly the reference pages it links, in that
  order, and nothing else;
- `adapt.mjs` derives the site's reference-tier sidebar order from the same
  list.

That is deliberate: one manifest is what stops the two outputs drifting.

**A reference page not linked from `index.md` is invisible to the in-app book.**
It will not appear, and nothing will fail — the page simply is not in the
collection. If it is also an `F1` target, the coverage gate catches it, because
`test_help_registry.cpp` greps `index.md` for the literal `(<slug>.md)`. If it
is not an `F1` target, nothing catches it. Link the page.

## Syntax budget for `reference/`

Reference pages pass through **md4c** with `MD_DIALECT_GITHUB`, and the result
is rendered by `QTextBrowser`, which supports a limited HTML subset. The budget
is therefore the intersection of the two, and it is narrow.

**Works:** headings, paragraphs, emphasis, lists, blockquotes, fenced and
indented code, links, images, **tables**, **strikethrough**, **task lists**, and
bare-URL autolinks.

**Does not exist — do not use:**

| Not available | Why |
|---|---|
| Footnotes | md4c has no footnote support at all |
| YAML front matter | nothing strips it; it renders as literal text at the top of the page |
| `:::note` and other admonitions | Starlight syntax; the in-app renderer prints the colons |
| Anything a code fence's *info string* is supposed to trigger | the fence renders as plain preformatted text; the language is not read |
| LaTeX math, wiki links, `__underline__` | not in `MD_DIALECT_GITHUB` |

### No heading anchors — the one that bites

The in-app renderer emits **no `id` attributes on headings**. A link written as
`page.md#section` therefore lands at the **top** of the target page in the app,
while working correctly on the site.

That is not a bug to route around; it is a constraint to design for. Link to a
page, not to a section within it. If you find yourself needing to point at one
section of a long reference page, the page probably wants splitting.

### `<kbd>` — settled: use backticks

Seven pages use `<kbd>Shift</kbd>` inline HTML; the rest use `` `Shift` `` for
the same thing.

**The rule is backticks.** Three reasons: `QTextBrowser` renders `<kbd>`
unstyled, so in the app it looks exactly like the surrounding prose and the
distinction it was reaching for is lost; backticks are pure CommonMark and
render identically in the app, on the site, and on GitHub; and it is already the
majority spelling.

Write `` `Shift`+`L` ``, not `<kbd>Shift</kbd>+<kbd>L</kbd>`.

*Normalising the existing seven pages is a separate change — this page settles
the rule, it does not apply it.*

## Syntax budget for guides

Markdown plus **Starlight asides**:

```markdown
:::note
Useful but skippable.
:::

:::caution[Watch out]
Something that will cost you time if you miss it.
:::
```

**MDX and JavaScript components stay deferred.** The reason is the offline
reader: the manual bundled in every release opens from `file://`, and keeping
guides to Markdown plus asides keeps that build static, portable, and free of
anything that needs a server or a hydration runtime. Ask before reaching for
more.

## The reference → guide bridge

A reference page may end with a section under this exact heading:

```markdown
## Full guide

[Shaping lanes](../tutorials/shaping-lanes.md) — the whole cross-section pass.
```

The **heading** is the marker, so what you write stays an ordinary relative
Markdown link that works on GitHub. Each generator retargets it: the site emits
a normal link, and the in-app book emits a URL that opens the packaged manual in
your system browser.

Rules:

- exactly one bridge section per page, as the **last** section;
- the **first** link in it is the bridge; prose after it is fine;
- the target must be a real page in the guides tier.

**CI verifies the target exists**, from both sides:
`HelpBridge.EveryBridgeTargetIsAPageThatExists` checks it in C++ against
`docs/user-guide/`, and the site adapter fails on the same broken link. Renaming
a tutorial without updating the pages that bridge to it fails the build twice.

## Images

Put the image beside the page that uses it, in that tier's `img/` folder:

| Page | Image |
|---|---|
| `reference/create-road.md` | `reference/img/create-road.png` |
| `tutorials/shaping-lanes.md` | `tutorials/img/shaping-lanes.png` |
| `index.md` | `img/…` |

Reference it relatively: `![Alt text](img/create-road.png)`.

Two constraints come from the in-app pipeline, and both are silent when broken:

- **`png`, `gif` and `jpg` only.** An `svg` will not be bundled. (A `.gif` is
  bundled but will not animate in `QTextBrowser` — treat it as a still.)
- **The `<files>` patterns do not recurse.** `helpc/qhp.cpp` lists
  `img/*.png`, `reference/img/*.png` and so on, explicitly. **A new image folder
  needs its own patterns added there**, or its images are simply absent from the
  in-app book while the page renders fine on the site.

Always write alt text: it is what a screen reader has, and what shows when an
image is missing.

## Checking your work

```sh
cd docs-site
npm ci                              # once
npm run build:web -- --base=/dev/   # the site, as published
npm run build:local                 # the offline reader
npm run check:links                 # every internal link and image resolves
```

The adapter **fails** on a broken link and names the page and the target. The
C++ side is covered by the editor test suite:

```sh
cmake --build --preset dev-macos --target roadmaker_editor_tests
ctest --preset dev-macos -R 'Help|Manual'
```

To see your reference page in the app, build the editor and press `F1`.

## The end-to-end walkthrough

Adding one page of each tier, start to finish — no steps beyond this page:

1. `docs/user-guide/reference/my-tool.md`, beginning `# My Tool`.
2. `docs/user-guide/tutorials/using-my-tool.md`, beginning `# Using My Tool`.
3. In `docs/user-guide/index.md`, add a row to the tools table linking
   `reference/my-tool.md`, and a row to the tutorials table linking
   `tutorials/using-my-tool.md`.
4. Optionally end the reference page with a `## Full guide` section linking
   `../tutorials/using-my-tool.md`.
5. `cd docs-site && npm run build:web -- --base=/dev/` — both pages appear on
   the site, the reference one in the Reference sidebar at the position its
   `index.md` row occupies.
6. Build the editor — the reference page is in the in-app book; the tutorial is
   **not**, which is the tier split working.

## See also

- [Publishing the documentation site](docs-site-publishing.md) — how a merged
  change reaches a live page.
- [`docs-site/README.md`](https://github.com/Robomous/RoadMaker/blob/main/docs-site/README.md)
  — the two builds, the adapter, and the licence gate.
- [ADR-0009](../decisions/0009-documentation-site-tiered-docs.md) — why the
  model is what it is.
