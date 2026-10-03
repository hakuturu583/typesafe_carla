#!/usr/bin/env python3
"""Bumps the release version in every place that has to agree on it.

The package version lives in two files, and a test (tests/test_architecture.py)
and the release workflow refuse to go on unless they match:
`python/typesafe_carla/__init__.py` (what PyPI sees, read by scikit-build-core)
and `codon/typesafe_carla/__init__.codon` (what Codon programs see). The
toolchain's version (`toolchain/pyproject.toml`) follows Codon instead and is
never touched here. The auto-release job calls this rather than editing by hand.

    tools/bump_version.py patch          # 0.1.0 -> 0.1.1
    tools/bump_version.py minor          # 0.1.0 -> 0.2.0
    tools/bump_version.py major          # 0.1.0 -> 1.0.0
    tools/bump_version.py 1.4.2          # set exactly

The single line printed to stdout is the new version, which the workflow reads
back to form the tag. Nothing else is printed there, so it can be captured
directly.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILES = (
    Path("python/typesafe_carla/__init__.py"),
    Path("codon/typesafe_carla/__init__.codon"),
)
_LINE = re.compile(r'(?m)^(__version__ = )"([^"]+)"')


def current_version(root: Path = ROOT) -> str:
    versions = {}
    for rel in VERSION_FILES:
        match = _LINE.search((root / rel).read_text())
        if match is None:
            raise SystemExit(f"could not find __version__ in {rel}")
        versions[str(rel)] = match.group(2)
    if len(set(versions.values())) != 1:
        raise SystemExit(f"the package versions disagree: {versions}")
    return next(iter(versions.values()))


def next_version(current: str, spec: str) -> str:
    if spec not in {"major", "minor", "patch"}:
        if not re.fullmatch(r"\d+\.\d+\.\d+", spec):
            raise SystemExit(f"expected major|minor|patch or an X.Y.Z version, got {spec!r}")
        return spec
    if not re.fullmatch(r"\d+\.\d+\.\d+", current):
        raise SystemExit(f"cannot bump {current!r}: not an X.Y.Z version; give one explicitly")
    major, minor, patch = (int(part) for part in current.split("."))
    if spec == "major":
        return f"{major + 1}.0.0"
    if spec == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def set_version(version: str, root: Path = ROOT) -> None:
    for rel in VERSION_FILES:
        path = root / rel
        text, count = _LINE.subn(rf'\g<1>"{version}"', path.read_text(), count=1)
        if count != 1:
            raise SystemExit(f"failed to rewrite __version__ in {rel}")
        path.write_text(text)


def main(argv: list[str] | None = None, root: Path = ROOT) -> None:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        raise SystemExit(__doc__)
    version = next_version(current_version(root), argv[0])
    set_version(version, root)
    print(version)


if __name__ == "__main__":
    main()
