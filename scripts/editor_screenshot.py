#!/usr/bin/env python3
"""Render an editor viewport screenshot of a .xodr scene, headless-friendly.

Thin wrapper over the editor's ``--screenshot`` mode that (a) locates the
editor binary across the per-platform build layouts, (b) maps the editor's
"no GL available" exit code to a skip instead of a failure when asked
(CI's offscreen runners may lack GL — the visual-artifact job must
skip-not-fail, docs/contributing/ci.md).

Usage:
  python scripts/editor_screenshot.py scene.xodr out.png
      [--camera top|orbit] [--size WxH] [--editor PATH] [--skip-on-no-gl]
      [--select ODR_ID] [--hover ODR_ID]

``--select``/``--hover`` highlight a road by its OpenDRIVE id so the viewport
feedback states (selection tint, hover brighten) render in the capture.

Exit codes: 0 ok (or skipped with --skip-on-no-gl), 1 failure, 3 no GL.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The editor's --screenshot exit code for "GL init failed" (editor/src/main.cpp).
NO_GL_EXIT = 3

CANDIDATE_GLOBS = [
    # macOS app bundles (any preset).
    "build/*/editor/roadmaker-editor.app/Contents/MacOS/roadmaker-editor",
    "build/editor/roadmaker-editor.app/Contents/MacOS/roadmaker-editor",
    # Linux / flat layouts.
    "build/*/editor/roadmaker-editor",
    "build/editor/roadmaker-editor",
    # Windows.
    "build/*/editor/roadmaker-editor.exe",
    "build/*/editor/*/roadmaker-editor.exe",
]


def find_editor() -> Path | None:
    candidates: list[Path] = []
    for pattern in CANDIDATE_GLOBS:
        candidates.extend(p for p in REPO_ROOT.glob(pattern) if p.is_file())
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--camera", choices=["top", "orbit"], default="orbit")
    parser.add_argument("--size", default="1600x1000")
    parser.add_argument("--editor", type=Path, default=None)
    parser.add_argument("--select", default=None, help="OpenDRIVE id to select (selection tint)")
    parser.add_argument("--hover", default=None, help="OpenDRIVE id to hover (hover brighten)")
    parser.add_argument("--tool", default=None, help="tool id to activate (renders its handles)")
    parser.add_argument("--ui", action="store_true", help="capture the whole themed window")
    parser.add_argument("--raise-dock", default=None, help="dock objectName to raise (with --ui)")
    parser.add_argument("--toast", default=None, help="toast text to show (renders the toast)")
    parser.add_argument(
        "--skip-on-no-gl",
        action="store_true",
        help="exit 0 when the editor reports no usable OpenGL (CI runners)",
    )
    args = parser.parse_args()

    editor = args.editor or find_editor()
    if editor is None or not editor.is_file():
        print("editor_screenshot: no editor binary found (build first)", file=sys.stderr)
        return 1

    cmd = [
        str(editor),
        "--screenshot-ui" if args.ui else "--screenshot",
        str(args.scene),
        str(args.out),
        "--camera",
        args.camera,
        "--size",
        args.size,
    ]
    if args.select:
        cmd += ["--select", args.select]
    if args.hover:
        cmd += ["--hover", args.hover]
    if args.tool:
        cmd += ["--tool", args.tool]
    if args.raise_dock:
        cmd += ["--raise-dock", args.raise_dock]
    if args.toast:
        cmd += ["--toast", args.toast]
    result = subprocess.run(cmd, env=os.environ.copy(), check=False)
    if result.returncode == NO_GL_EXIT and args.skip_on_no_gl:
        print("editor_screenshot: no GL on this runner — skipped (not a failure)")
        return 0
    if result.returncode == 0 and not args.out.is_file():
        print("editor_screenshot: editor exited 0 but wrote no image", file=sys.stderr)
        return 1
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
