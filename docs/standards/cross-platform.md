# Cross-platform rules

*How RoadMaker stays correct on macOS, Linux, and Windows. Consult this before writing filesystem, locale, GL, or OS-conditional code, and before touching CI or deployment.*

Development happens primarily on macOS, but Linux and Windows are first-class:
every PR must pass all three OS jobs in CI — no "fix Windows later" merges.
Any platform-conditional code (`#if defined(...)`, per-OS CMake branches)
needs a comment explaining *why* the platforms differ.

## Filesystem and text

- Use `std::filesystem::path` end-to-end. Convert to strings only at API
  edges, always as UTF-8 (`path.u8string()` — mind `char8_t`; the kernel
  provides `rm::to_utf8` / `rm::from_utf8` helpers). Never concatenate paths
  as strings.
- Open files in binary mode explicitly. Never assume `'\n'` on disk: the
  `.xodr` parser must accept CRLF line endings.
- Make no case-sensitivity assumptions in file lookups — macOS and Windows
  filesystems are usually case-insensitive, Linux is not.

## Language and runtime portability

- No POSIX-only APIs in `core/` (no `unistd.h`, no `fork`). If unavoidable in
  editor or tooling code, isolate it in a `platform_*.cpp` file with a comment
  saying why.
- MSVC gotchas to respect:
  - No VLAs (a GCC/Clang extension; we build with extensions off anyway).
  - `min`/`max` macro clash — `NOMINMAX` is defined globally, keep it that
    way.
  - Two-phase lookup is real on modern MSVC: keep templates standard-clean
    (dependent names properly qualified, `typename`/`template` where
    required).
  - Avoid the `and`/`or` alternative tokens.
- Never rely on struct layout, packing, or bitfields for serialization —
  write explicit encoders and decoders.

## Locale-safe number IO

**This is the #1 source of cross-platform `.xodr` bugs.** `std::stod`,
`atof`, `sscanf`, and iostream extraction all respect the process locale — on
a comma-decimal locale (`de_DE`, `fr_FR`, …) they silently misparse
`"3.14"`. Rules:

- Parse numbers with `std::from_chars` or fast_float (the pinned
  `FastFloat::fast_float` target exists because `std::from_chars` for
  `double` is not yet universal across our CI standard libraries).
- Format numbers with fmt.
- Never use any locale-dependent conversion in `.xodr` (or any file-format)
  IO, in either direction.

## Qt and OpenGL (editor only)

The layer rules — Qt and GL confined to the editor — are defined in the
[architecture overview](../architecture/overview.md). The platform-specific
consequences:

- **OpenGL 3.3 core profile**, requested via
  `QSurfaceFormat::setDefaultFormat` in `main()` **before** constructing
  `QApplication` — otherwise macOS hands out a legacy 2.1 context. Use no GL
  features above 4.1 (macOS's ceiling). GL functions are resolved through the
  injected `ProcResolver`
  (`QOpenGLContext::currentContext()->getProcAddress`), never GLFW or glad.
- **HiDPI:** GL viewport size = widget size × `devicePixelRatioF()`. Picking
  math may stay in widget units — the ratio cancels against the widget-unit
  viewport size.
- **Headless tests** run with `QT_QPA_PLATFORM=offscreen`. The offscreen
  platform provides no real GL 3.3 context, so never unit-test `paintGL` or
  the GL renderer — packaging smoke tests cover them. See
  [contributing/testing](../contributing/testing.md).
- **Config paths:** QSettings storage differs per OS (registry / plist / XDG
  ini). Never hardcode a config path; always go through `QSettings`.

## Deployment per OS

Qt deployment is per-platform and runs at install time:

- **Windows** (`windeployqt`) and **macOS** (`macdeployqt`) are wired via
  `qt_generate_deploy_app_script`, which **must** be called from the directory
  where `find_package(Qt6)` ran — its support variables are directory-scoped.
- `macdeployqt` does **not** copy non-Qt shared libraries: the kernel dylib is
  installed into `Contents/Frameworks` explicitly, as a real file renamed to
  its soname (installing the symlink ships a dangling link).
- **Linux** uses `linuxdeploy` plus its Qt plugin in the release workflow.
- Qt 6.8 + current macOS SDKs: `FindWrapOpenGL` still links the removed AGL
  framework; `editor/CMakeLists.txt` strips every AGL entry from
  `WrapOpenGL::WrapOpenGL` (Apple removed AGL from modern SDKs, so linking
  fails otherwise).

## Build and CI conventions

- **Ninja everywhere** — CI and documented local builds use the Ninja
  generator (build commands: [getting started](../getting-started/building.md)).
- ccache/sccache is enabled in CI; FetchContent downloads are cached keyed on
  the hash of `cmake/deps.cmake`.
- **Sanitizers run on Linux clang only** (ASan+UBSan): they are unreliable on
  macOS CI runners and unsupported under MSVC — the Windows job uses
  `/analyze` instead.
- Every PR must pass the macOS, Linux, and Windows jobs before merge.
