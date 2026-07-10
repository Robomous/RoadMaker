# Building RoadMaker

*Canonical build instructions for all three platforms: provisioning Qt, configuring with CMake presets, building, and testing.*

## Prerequisites

- CMake ≥ 3.26 and Ninja (all presets use the Ninja generator)
- A C++20 compiler: AppleClang (macOS), GCC or Clang (Linux), MSVC 2022 (Windows)
- Python 3 (for the Qt provisioning script and the optional Python bindings)

All other dependencies are fetched and pinned by the build itself (see
[dependency policy](../standards/dependencies.md)) — you install nothing else
by hand, including Qt.

## One-time: provision Qt

The editor links Qt 6 dynamically. A script downloads the pinned Qt version
into the gitignored `./.qt/` directory; CMake discovers it automatically:

```sh
python3 scripts/setup_qt.py
```

The Qt version pin lives in a single place, `cmake/QtVersion.cmake`
(currently `set(RM_QT_VERSION 6.8.3)`). The script is idempotent — rerunning
it when the pinned version is already present just prints the install prefix.
If you only build the kernel (no editor), skip this step entirely.

## Configure, build, test

Pick the preset for your OS — `dev-macos`, `dev-linux`, or `dev-windows`:

```sh
cmake --preset dev-macos
cmake --build --preset dev-macos
ctest --preset dev-macos
```

```sh
# Linux
cmake --preset dev-linux && cmake --build --preset dev-linux && ctest --preset dev-linux
```

```sh
# Windows (run from a "x64 Native Tools" VS 2022 prompt so cl.exe is on PATH)
cmake --preset dev-windows && cmake --build --preset dev-windows && ctest --preset dev-windows
```

Each preset builds into `build/<preset-name>/`. The `dev-*` presets use
`RelWithDebInfo` and enable tests and the editor.

### Available presets

| Preset family | Purpose | Notable cache variables |
|---|---|---|
| `dev-macos` / `dev-linux` / `dev-windows` | Local development | `RelWithDebInfo`, `RM_BUILD_TESTS=ON`, `RM_BUILD_EDITOR=ON` |
| `ci-macos` / `ci-linux` / `ci-windows` | What [CI](../contributing/ci.md) runs | `Release`, warnings-as-errors (`RM_WERROR=ON`) |
| `release-macos` / `release-linux` / `release-windows` | Installer packaging | `Release`, `RM_BUILD_SHARED=ON`, `RM_BUILD_PYTHON=OFF` |

Every configure preset has matching build and test presets of the same name.

## Build options

| Option | Default | Effect |
|---|---|---|
| `RM_BUILD_TESTS` | `ON` | Build the GoogleTest suites |
| `RM_BUILD_EDITOR` | `OFF` (presets: `ON`) | Build the Qt 6 Widgets editor |
| `RM_BUILD_PYTHON` | `OFF` | Build the nanobind Python module (usually built via `pip` instead — see [Running](running.md)) |
| `RM_BUILD_SHARED` | `OFF` | Build `roadmaker_core` as a shared library with install rules |
| `RM_BUILD_FUZZERS` | `OFF` | Build libFuzzer targets (Clang only) |
| `RM_BUILD_USD` | `OFF` | OpenUSD exporter — currently a stub pending M2 phase 5 ([design](../design/m2/04_usd_export.md)) |

### Kernel-only build

No Qt, no editor — useful for servers and for consuming the kernel as a
library:

```sh
cmake -B build -G Ninja -DRM_BUILD_EDITOR=OFF -DRM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Shared kernel and `find_package(roadmaker)`

`-DRM_BUILD_SHARED=ON` builds `roadmaker_core` as a shared library with
install rules. Third-party projects can then consume it as a normal CMake
package:

```sh
cmake -B build -G Ninja -DRM_BUILD_SHARED=ON -DRM_BUILD_EDITOR=OFF
cmake --build build
cmake --install build --prefix /path/to/install
```

```cmake
find_package(roadmaker REQUIRED)
target_link_libraries(my_app PRIVATE roadmaker::core)
```

A minimal consumer project lives in `tests/consume_installed/` and is built
against the installed package in CI. Python wheels always embed the static
kernel regardless of this option.

### Sanitizer build

Run an ASan+UBSan build before merging anything that touches geometry or
parsing (see [Testing](../contributing/testing.md)):

```sh
cmake -B build-asan -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DRM_BUILD_TESTS=ON -DRM_SANITIZE=address,undefined
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Sanitizers are most reliable on Linux with Clang; that is also what CI runs.

## Troubleshooting

**CMake cannot find Qt6.** Run `python3 scripts/setup_qt.py`, then delete the
stale configure cache (`rm -rf build/<preset>`) and reconfigure —
`CMAKE_PREFIX_PATH` is computed at configure time from the provisioned tree.
If the pin in `cmake/QtVersion.cmake` changed since you last provisioned,
rerunning the script downloads the new version.

**Linux: Qt fails at configure/test time with missing xcb/GL libraries.**
Install the runtime packages the CI job uses:

```sh
sudo apt-get install -y libgl1-mesa-dev libegl1 libfontconfig1 \
  libxkbcommon-x11-0 libdbus-1-3 libglib2.0-0t64 \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
  libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
  libxcb-xinerama0 libxcb-xkb1
```

**Windows: "no CMAKE_CXX_COMPILER could be found".** The Ninja generator
needs the MSVC environment. Use a "x64 Native Tools Command Prompt for VS
2022" (or call `vcvars64.bat`) before configuring.

**Windows: editor tests fail to start during the build.** Test discovery runs
the editor test executable, which must be able to load the Qt DLLs. Put the
provisioned Qt `bin` directory (`.qt\6.8.3\msvc2022_64\bin`) on `PATH`.

**Stale build after switching branches or options.** Preset builds are
self-contained per directory; deleting `build/<preset>/` and reconfiguring is
always safe.
