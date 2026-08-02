# Publishing the documentation site

How the web manual gets from a merged PR to a live page, and what to do when it
does not. Written for the maintainer: you should not have to read the workflow
YAML to run any of this.

Decided in [ADR-0009](../decisions/0009-documentation-site-tiered-docs.md).
The offline reader that ships inside a release is a different build — see
[the docs-site README](https://github.com/Robomous/RoadMaker/blob/main/docs-site/README.md).

## What gets published

The hosting app serves one branch, **`docs-published`**, exactly as it stands.
GitHub Actions assembles that branch:

```
/                 redirect to latest/, or to dev/ while no release exists
/versions.json    the manifest the header dropdown reads
/dev/             built from main
/vX.Y.Z/          built from each release tag
/latest/          a copy of the highest-semver version
/amplify.yml      the serve-prebuilt config
```

**Nothing in this pipeline creates a tag or a release.** The workflow reacts to a
tag you have already pushed. Publishing a release is your decision and yours
alone ([release philosophy](../roadmap/README.md#release-philosophy)).

Today only `dev/` exists, because no release has been tagged since the roadmap
reset. That is a supported state, not a half-built one: the root redirect points
at `dev/`, and the version dropdown hides itself rather than offering a choice
of one.

## One-time setup — connecting the hosting app

Do this once. Everything here is on your side; no automation has, or should
have, credentials for it.

1. **Let the branch be created.** Merge anything that touches `docs/user-guide/`
   or `docs-site/`, or run the workflow manually. The first run creates
   `docs-published` as an orphan branch — you do not seed it by hand. Confirm it
   exists and has `index.html`, `versions.json` and `dev/` at its root.
2. **Create the hosting app** against this repository and **select the
   `docs-published` branch**. Not `main`.
3. **Do not enable a build.** The branch is already the finished site.
   `amplify.yml` at its root declares empty build phases and
   `baseDirectory: /`, so the app publishes the branch as it stands. If the
   console offers to detect a framework, decline it — a detected Astro build
   would try to build the *published output* as if it were source.
4. **Permissions:** read access to the repository and to that one branch.
   Nothing needs write access; the workflow pushes with the repository's own
   `GITHUB_TOKEN`.
5. **Domain.** Attach the domain to the app once a deploy is green. The tree is
   root-relative, so it works under any hostname; it does **not** work under a
   path prefix, because each version is already a path segment.
6. **Verify** with the checklist below.

## The everyday flow

| You do | What happens |
|---|---|
| Merge a docs change to `main` | `dev/` is rebuilt and pushed to `docs-published`; the host redeploys |
| Push a tag `vX.Y.Z` | `vX.Y.Z/` is built, `latest/` is recomputed as the **highest semver**, the root redirect and `versions.json` are regenerated |
| Nothing | Nothing. An assembly that produces identical bytes commits nothing and triggers no deploy |

`latest/` follows the **highest version number, not the most recent tag**. If you
ever patch an old line — tagging `v0.9.1` after `v0.10.0` exists — `latest/`
stays on `v0.10.0`, which is almost certainly what you want. It is a copy rather
than a link because the host serves files, and a symlink committed to git is
just a file containing a path.

## Rehearsing a release before you tag one

The tag-driven path is the one you will use least and can least afford to have
wrong. Rehearse it:

**Actions → docs publish → Run workflow**, enter a version such as `v0.1.0`.

The run is labelled `DRY RUN … — scratch prefix, publishes nothing` in the
Actions list, and its summary says the same. It assembles a complete tree —
version directory, recomputed `latest/`, regenerated `versions.json`, root
redirect — under `_dryrun/` on the publishing branch.

It cannot damage the live tree, and that holds two ways. The assembler is given
`_dryrun/` as its root, so the real `dev/`, `latest/`, `versions.json` and root
redirect are not addressable from inside it at all. And a step afterwards asks
**git** what changed and fails the run if anything outside `_dryrun/` did —
because a dry run that quietly republished `dev/` would otherwise look exactly
like a passing one.

Inspect `_dryrun/` on the `docs-published` branch, then delete it whenever you
like; nothing reads it.

## Verifying a deploy

1. The site root redirects to `latest/` if a release exists, else to `dev/`.
2. `‹site›/versions.json` lists every version directory, with `latest` naming
   the highest release (or `null` before the first one).
3. Search returns results — search is on for the web build, off only for the
   offline reader that ships in a release.
4. Open a page **inside** a section, e.g. `‹site›/dev/reference/junction/`, and
   follow a link in the body text, not in the sidebar. This is the check worth
   doing by hand: Astro prefixes the links it generates, so a broken base leaves
   the sidebar working perfectly and only breaks links written in the guide's
   own Markdown. CI gates it (`check-web-build.mjs`), but this is what the gate
   is protecting.
5. With two or more versions, the header dropdown switches version and **keeps
   you on the same page** where that page exists in the target, and lands on
   that version's front page where it does not. It reads the page list out of
   `versions.json` rather than probing the server, so a host that answers a
   missing file with a 200 fallback cannot make it look right while sending
   readers nowhere.

## When an assembly run fails

Read which step failed first; the failure modes are distinct.

- **`Licence gate`** — a dependency outside the permitted set reached the tree.
  This is a dependency-policy decision, not a docs problem. Do not bypass it;
  see [the dependency policy](../standards/dependencies.md).
- **`Build the site for this segment`** — the adapter found a broken link, an F1
  page missing from the site, or the base check found a root-absolute reference
  without the version prefix. The message names the page and the target. Fix it
  on `main`; the site rebuilds on merge.
- **`Prepare the publishing branch`** — a `docs-published` that is not what the
  workflow expects. Safe to fix by hand: the branch is entirely derived output.
  Deleting it makes the next run recreate it from scratch, at the cost of losing
  the version directories of releases that will not be rebuilt. Prefer to repair
  it before deleting it.
- **`Assemble`** — the segment was refused, or `docs-site/dist` had no
  `index.html`. Both are bugs in the plan step and should not reach you.
- **`Commit and push`** — usually a concurrent run. Re-run; the workflow
  serialises on a concurrency group and assembly is idempotent, so a repeat is
  harmless.
- **The host does not redeploy** — check the app is watching `docs-published`
  and that the last workflow run actually pushed a commit. An assembly whose
  output is byte-identical commits nothing on purpose, so "no new deploy" after
  a no-op change is correct behaviour.

Re-running any assembly is safe. It replaces one version directory and
recomputes the derived files from whatever is on the branch, so a version
published by an earlier run survives a run that knows nothing about it.

## Where the pieces live

| Piece | Path |
|---|---|
| Assembly workflow | `.github/workflows/docs-publish.yml` |
| Assembler (with its tests) | `docs-site/scripts/assemble.mjs`, `docs-site/test/assemble.test.mjs` |
| Web build | `docs-site/scripts/build-web.mjs` |
| Base gate | `docs-site/scripts/check-web-build.mjs` |
| Version dropdown | `docs-site/src/components/VersionSelect.astro` |
| Serve-prebuilt config | `docs-site/publish/amplify.yml` |
