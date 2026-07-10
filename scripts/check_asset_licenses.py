#!/usr/bin/env python3
"""CI lint: every repository asset is accounted for in ASSETS_LICENSES.md.

Checks (policy: docs/m2/05_assets.md):
1. Every file under assets/ and editor/resources/ has a row in
   ASSETS_LICENSES.md naming its repo-relative path.
2. Every assets/manifest.json entry uses an allowed license.

Exemptions:
- assets/samples/ — project-authored .xodr test data covered by the repo
  LICENSE (not media assets).
- assets/manifest.json and *.qrc — machinery, not assets.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LEDGER = REPO_ROOT / "ASSETS_LICENSES.md"
MANIFEST = REPO_ROOT / "assets" / "manifest.json"
SCAN_ROOTS = ("assets", "editor/resources")
EXEMPT_DIRS = ("assets/samples",)
EXEMPT_NAMES = {"manifest.json"}
EXEMPT_SUFFIXES = {".qrc"}


def is_exempt(relative: str, path: Path) -> bool:
    if any(relative == d or relative.startswith(d + "/") for d in EXEMPT_DIRS):
        return True
    return path.name in EXEMPT_NAMES or path.suffix in EXEMPT_SUFFIXES


def main() -> int:
    ledger_text = LEDGER.read_text(encoding="utf-8")
    errors: list[str] = []

    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(REPO_ROOT).as_posix()
            if is_exempt(relative, path):
                continue
            if f"`{relative}`" not in ledger_text:
                errors.append(f"missing ASSETS_LICENSES.md row: {relative}")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    allowed = set(manifest["allowed_licenses"])
    for entry in manifest["assets"]:
        if entry["license"] not in allowed:
            errors.append(
                f"manifest entry {entry['destination']}: license "
                f"{entry['license']!r} not in allowed set {sorted(allowed)}")

    if errors:
        for error in errors:
            print(f"[check_asset_licenses] {error}")
        return 1
    print("[check_asset_licenses] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
