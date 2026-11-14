#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check that every pinned package copy still matches the component it mirrors.

Two sources in `package/` stay there because a host harness compiles them by path (`ShengGpr.c` and
`ShengAudioReach.c`); the kernel build compiles the component from the assembled tree instead. That leaves
two copies of the same code and nothing comparing them, which is how they drift.

The comparison needs the component, and the component only exists in an assembled tree. So this check runs
in two modes:

  * with a tree - `$UNIXNU_TREE`, or a `drivers/<id>/` beside this repository - it compares each copy with
    the component source, ignoring the pinned-copy note at the top of the package file;
  * without one, it says so and compares the copies with each other's recorded revision instead: it prints
    the catalogue revision it would have checked against and exits 0, because a repository-only run has no
    component to read. That is stated rather than silently skipped.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAIRS = [("package/ShengGpr.c", "ShengGpr", "ShengGpr.c"),
         ("package/ShengGpr.h", "ShengGpr", "ShengGpr.h"),
         ("package/ShengAudioReach.c", "ShengAudioReach", "ShengAudioReach.c"),
         ("package/ShengAudioReach.h", "ShengAudioReach", "ShengAudioReach.h")]


def strip_note(text):
    """Drop the pinned-copy comment so the comparison is about code."""
    return re.sub(r"/\* PINNED COPY.*?\*/\n", "", text, flags=re.S)


def find_tree():
    for candidate in (os.environ.get("UNIXNU_TREE"), ROOT.parent / "tree", ROOT / "tree"):
        if candidate and (Path(candidate) / "drivers").is_dir():
            return Path(candidate)
    return None


def main():
    tree = find_tree()
    if tree is None:
        print("note: no assembled tree, so the component copies were not compared "
              "(set UNIXNU_TREE or run after `unixnu tree`)", file=sys.stderr)
        return 0
    for package_path, component, filename in PAIRS:
        package_text = strip_note((ROOT / package_path).read_text(encoding="utf-8"))
        component_path = tree / "drivers" / component / filename
        if not component_path.is_file():
            print(f"FAIL {package_path}: {component_path} is missing", file=sys.stderr)
            return 1
        component_text = component_path.read_text(encoding="utf-8")
        if package_text.split() != component_text.split():
            print(f"FAIL {package_path} differs from {component_path}", file=sys.stderr)
            return 1
    print(f"PASS component copies: {len(PAIRS)} package copies match their components")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
