#!/usr/bin/env python3
"""Validate a .usda file against the OpenUSD reference compliance checker.

Wraps ``pxr.UsdUtils.ComplianceChecker`` — the same engine the ``usdchecker``
CLI historically used — so validation works from a plain ``pip install
usd-core``, which ships the pxr Python modules but *not* the command-line tools
(``usdchecker``/``usdcat``/``usdview``). Errors and failed checks fail the run;
warnings are printed but do not (mirrors the default ``usdchecker`` behavior).

Usage:
    python usd_compliance_check.py <file.usda>
"""

import sys

from pxr import UsdUtils


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <file.usda>")
        return 2
    path = sys.argv[1]

    checker = UsdUtils.ComplianceChecker()
    checker.CheckCompliance(path)

    for warning in checker.GetWarnings():
        print(f"WARN: {warning}")

    problems = list(checker.GetErrors()) + list(checker.GetFailedChecks())
    for problem in problems:
        print(f"ERROR: {problem}")

    if problems:
        print(f"FAILED: {path} is not USD-compliant ({len(problems)} issue(s))")
        return 1
    print(f"OK: {path} passed OpenUSD compliance checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
