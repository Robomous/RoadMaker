# Dependencies and licensing

*The license and dependency policy for this MIT-licensed project. Read this before adding, upgrading, or vendoring any third-party code — including snippets copied from other projects.*

RoadMaker ships under MIT. Everything it links, embeds, or redistributes must
be compatible with that story. When you are unsure about a license, **stop and
ask the maintainer** — do not add the dependency and sort it out later.

## Allowed licenses

MIT, BSD-2/BSD-3, Apache-2.0, MPL-2.0 (its file-level copyleft is fine),
zlib, BSL-1.0, Unlicense/CC0.

## Forbidden

- **GPL** (any version) — includes GPL-licensed *subsets* of otherwise-fine
  libraries (e.g., libigl's `copyleft/` headers are never included even
  though libigl itself is pinned).
- **LGPL** — static-link risk; see the single sanctioned exception below.
- **AGPL**, **SSPL**.
- **Proprietary SDKs** (e.g., the Autodesk FBX SDK).
- **Unlicensed code**, including Stack Overflow snippets — reimplement
  instead of copying.

## The sanctioned exception: Qt

Qt 6 is the **only** LGPL dependency, used under LGPLv3 for the editor, under
hard conditions (see [ADR 0003](../decisions/0003-qt-widgets-editor.md)):

- **Dynamic linking only.** Never build or link static Qt — static linking
  would drag the application into LGPL obligations incompatible with the MIT
  story.
- **Editor targets only.** `core/` and `python/` never include a Qt header or
  link a Qt library; the kernel and the Python wheels stay pure MIT.
- **Never vendored, never modified, never FetchContent.** Qt is provisioned by
  `scripts/setup_qt.py` (aqtinstall) into the gitignored `./.qt/` directory;
  the version pin lives in `cmake/QtVersion.cmake` and nowhere else.
- **Relink obligation in distributions:** every bundle ships the LGPLv3/GPLv3
  texts (checked in under `licenses/`), plus a notice in the About dialog and
  in `THIRD_PARTY_LICENSES.md` that users may replace the Qt libraries. The
  deploy tools keep Qt as separate shared libraries, which satisfies the
  provision.

Any *other* LGPL candidate still requires explicit maintainer approval before
it enters the tree.

## Explicit substitutions

Some obvious-looking libraries are forbidden and have designated
replacements. Do not "helpfully" add the forbidden one:

| Need | Use | Not |
|---|---|---|
| Robust booleans / solids | Manifold (Apache-2.0) | CGAL (GPL) |
| 2D triangulation / plan-view ops | CDT (MPL-2.0) + Clipper2 (BSL-1.0) | CGAL; Triangle (non-commercial-only!) |
| Scene export | glTF via tinygltf, later OpenUSD | FBX SDK (proprietary; FBX may become an out-of-tree plugin built against the user's own SDK) |

## Adding a dependency (checklist — all steps, one commit)

1. **Verify the license file in the upstream repository**, not just the
   README badge.
2. Pin it in `cmake/deps.cmake` with `FetchContent_Declare` using a
   **release-archive URL + `URL_HASH` SHA256**. No live git branches, ever.
   (Why FetchContent at all: [ADR 0002](../decisions/0002-fetchcontent.md).)
3. Set the dependency's build options to the minimum — no tests, examples, or
   tools.
4. Add a row to `THIRD_PARTY_LICENSES.md`: name, version, license, URL,
   usage.
5. Audit transitive dependencies it drags in — they must pass the same
   policy (and get noted; e.g., tinygltf's row records its bundled
   nlohmann/json and stb).

A real entry from `cmake/deps.cmake`:

```cmake
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz
  URL_HASH SHA256=8b852bb5aa6e7d8564f9e81394055395dd1d1936d38dfd3a17792a02bebd7af0
)
set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_DOC OFF)
```

## General rules

- Prefer header-only or small static libraries. Question anything that adds
  more than ~30 seconds to a clean build; discuss heavyweights before adding
  them (OpenUSD is pre-approved for M2 behind an OFF-by-default option until
  wheel packaging is solved).
- **Vendoring:** never commit vendored source unless the project is
  unmaintained *and* tiny. If vendoring, keep the upstream license header
  intact and record the exact upstream commit.
- **System packages** are only for CI tooling (ninja, ccache) — never for
  libraries the kernel links. Builds must be reproducible from CMake alone.
- **External smoke tools** are a distinct category from dependencies: a pinned
  third-party *binary* that CI runs as a subprocess to check our output, never
  links, never redistributes, and that no build or runtime path depends on.
  Because nothing is linked or shipped, the licence question is "may we run it",
  which is far weaker than the linking test the allowed-licence list exists for
  — but the tool still gets pinned to an exact version and its licence recorded
  at the call site.
  - **esmini** ([#51](https://github.com/Robomous/RoadMaker/issues/51)) — the
    OpenDRIVE round-trip gate. **MPL-2.0** (allowed above; verified against the
    upstream repository at close-out, 2026-07-15). Pinned `v3.5.0` in the
    `esmini-roundtrip` job, fetched as `esmini-bin_Linux.zip` from the GitHub
    release like a test fixture, cached, run `--headless`. It is never linked
    into any RoadMaker target and never redistributed. If it ever needed to be
    linked or shipped, MPL-2.0's file-level copyleft would apply and this entry
    would not cover it.
- Approved per-case exceptions are recorded in `THIRD_PARTY_LICENSES.md`
  and/or an ADR — e.g., tinyusdz's vendored ISC/Unlicense components are
  covered by [ADR 0005](../decisions/0005-tinyusdz-usda.md).

Asset (icon/texture/model) licensing has its own, stricter page:
[assets](./assets.md).
