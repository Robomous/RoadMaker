#!/usr/bin/env python3
"""Provision the Qt SDK the RoadMaker editor builds against.

Downloads Qt (qtbase + qttools) via aqtinstall into ./.qt/<version>/<dir>/ so
that neither developers nor CI ever install Qt by hand. Idempotent: if the
pinned version is already present, prints its prefix and exits.

The Qt version is pinned in cmake/QtVersion.cmake (the only place it lives);
cmake discovers the resulting install automatically, so after running this
script a plain `cmake --preset dev-<os>` works.

Licensing note: Qt is used under LGPLv3 with dynamic linking only, confined to
the editor target. See THIRD_PARTY_LICENSES.md.

Usage:
    python3 scripts/setup_qt.py [--version X.Y.Z] [--arch ARCH] [--output DIR]
"""

from __future__ import annotations

import argparse
import platform
import re
import subprocess
import sys
import venv
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
QT_VERSION_FILE = REPO_ROOT / "cmake" / "QtVersion.cmake"

# aqt host/arch names per platform. Arch names change between Qt minors
# (linux was gcc_64 before 6.7, windows was msvc2019 before 6.8); verify with
# `aqt list-qt <host> desktop --arch <version>` when bumping the pin.
HOST_CONFIG = {
    "Darwin": ("mac", "clang_64"),
    "Linux": ("linux", "linux_gcc_64"),
    "Windows": ("windows", "win64_msvc2022_64"),
}


def log(message: str) -> None:
    print(f"[setup_qt] {message}", flush=True)


def pinned_version() -> str:
    text = QT_VERSION_FILE.read_text(encoding="utf-8")
    match = re.search(r"^set\(RM_QT_VERSION (\d\S*)\)", text, re.MULTILINE)
    if not match:
        sys.exit(f"error: no `set(RM_QT_VERSION <x.y.z>)` line in {QT_VERSION_FILE}")
    return match.group(1)


# cmake package marker per required aqt archive; an install missing any of
# these (e.g. a tree provisioned before qtsvg was added) is re-provisioned.
REQUIRED_MODULE_MARKERS = {
    "qtbase": "lib/cmake/Qt6/Qt6Config.cmake",
    "qtsvg": "lib/cmake/Qt6Svg/Qt6SvgConfig.cmake",
}


def existing_prefix(output: Path, version: str) -> Path | None:
    for config in sorted(output.glob(f"{version}/*/lib/cmake/Qt6/Qt6Config.cmake")):
        prefix = config.parents[3]
        missing = [archive for archive, marker in REQUIRED_MODULE_MARKERS.items()
                   if not (prefix / marker).is_file()]
        if missing:
            log(f"Qt {version} at {prefix} lacks {', '.join(missing)}; reinstalling")
            return None
        return prefix
    return None


def venv_bin(venv_dir: Path, name: str) -> Path:
    subdir = "Scripts" if platform.system() == "Windows" else "bin"
    suffix = ".exe" if platform.system() == "Windows" else ""
    return venv_dir / subdir / f"{name}{suffix}"


def ensure_aqt(venv_dir: Path) -> Path:
    aqt = venv_bin(venv_dir, "aqt")
    if not aqt.exists():
        log(f"creating venv at {venv_dir}")
        venv.EnvBuilder(with_pip=True).create(venv_dir)
        pip = venv_bin(venv_dir, "pip")
        subprocess.run([str(pip), "install", "--quiet", "aqtinstall==3.*"], check=True)
    return aqt


def install_qt(aqt: Path, host: str, version: str, arch: str, output: Path) -> None:
    # qttools ships windeployqt/macdeployqt; qtsvg backs the editor's bundled
    # SVG icon set (Qt6::Svg). Adding a module here also means updating the
    # editor find_package() and checking the deploy expectations.
    archives = ["qtbase", "qttools", "qtsvg"]
    if host == "linux":
        archives.append("icu")  # Qt on Linux links a bundled ICU
    command = [
        str(aqt), "install-qt", host, "desktop", version, arch,
        "--outputdir", str(output),
        "--archives", *archives,
    ]
    log(" ".join(command))
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=None,
                        help="Qt version (default: pin in cmake/QtVersion.cmake)")
    parser.add_argument("--arch", default=None,
                        help="aqt arch name (default: per-platform)")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / ".qt",
                        help="install root (default: <repo>/.qt)")
    args = parser.parse_args()

    system = platform.system()
    if system not in HOST_CONFIG:
        sys.exit(f"error: unsupported platform {system}")
    host, default_arch = HOST_CONFIG[system]
    version = args.version or pinned_version()
    arch = args.arch or default_arch
    output = args.output.resolve()

    prefix = existing_prefix(output, version)
    if prefix is None:
        aqt = ensure_aqt(output / "venv")
        try:
            install_qt(aqt, host, version, arch, output)
        except subprocess.CalledProcessError:
            if args.arch is None and host == "linux" and arch == "linux_gcc_64":
                log("linux_gcc_64 not available for this version; retrying gcc_64")
                install_qt(aqt, host, version, "gcc_64", output)
            else:
                raise
        prefix = existing_prefix(output, version)
        if prefix is None:
            sys.exit(f"error: aqt reported success but no Qt6Config.cmake under {output}")
        log(f"installed Qt {version} to {prefix}")
    else:
        log(f"Qt {version} already installed at {prefix}")

    # Machine-readable last line; cmake/QtVersion.cmake discovers this path
    # itself, so exporting it is only needed for tooling outside the repo.
    print(f"CMAKE_PREFIX_PATH={prefix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
