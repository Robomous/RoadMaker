#!/usr/bin/env python3
# Copyright 2026 Robomous
# SPDX-License-Identifier: Apache-2.0

"""Fetch, verify, and place the external assets listed in assets/manifest.json.

Icons and processed textures are committed to the repository (small,
license-clean); this script exists to regenerate them, to upgrade upstream
versions, and to host anything too large to commit later. Policy and workflow:
docs/design/m2/05_assets.md. Stdlib only — must run on any CI runner.

Usage:
  python3 scripts/fetch_assets.py            # fetch everything missing
  python3 scripts/fetch_assets.py --force    # re-download even if present
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "assets" / "manifest.json"


def log(message: str) -> None:
    print(f"[fetch_assets] {message}")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(entry: dict, force: bool) -> bool:
    """Download one manifest entry. Returns True on success."""
    destination = REPO_ROOT / entry["destination"]
    if destination.exists() and not force:
        if sha256_of(destination) == entry["sha256"]:
            log(f"ok (present): {entry['destination']}")
            return True
        log(f"checksum drift, re-fetching: {entry['destination']}")

    log(f"downloading {entry['url']}")
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        urllib.request.urlretrieve(entry["url"], tmp_path)
        actual = sha256_of(tmp_path)
        if actual != entry["sha256"]:
            log(f"ERROR sha256 mismatch for {entry['destination']}:")
            log(f"  expected {entry['sha256']}")
            log(f"  actual   {actual}")
            return False
        if entry.get("postprocess"):
            # Postprocess steps are documentation of how the committed file
            # was derived (resize/re-encode command lines). They are applied
            # manually at asset-introduction time, not replayed here.
            log(f"note: {entry['destination']} has recorded postprocess steps; "
                "the raw download is placed as-is")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(tmp_path.read_bytes())
        log(f"placed {entry['destination']}")
        return True
    finally:
        tmp_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true",
                        help="re-download assets even when present and valid")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    allowed = set(manifest["allowed_licenses"])
    failures = 0
    for entry in manifest["assets"]:
        if entry["license"] not in allowed:
            log(f"ERROR {entry['destination']}: license {entry['license']!r} "
                f"not in allowed set {sorted(allowed)}")
            failures += 1
            continue
        if not fetch(entry, args.force):
            failures += 1

    if failures:
        log(f"{failures} asset(s) failed")
        return 1
    log(f"done ({len(manifest['assets'])} manifest entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
