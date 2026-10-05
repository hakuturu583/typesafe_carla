"""Sanity checks for a built typesafe-carla wheel, run against the installed package.

    python tools/check_wheel.py [--backend libcarla] [--carla-ref REF] [--carla-commit SHA]

Checks that the native library loads, exports the expected ABI, was built
with the expected backend (and CARLA ref and commit), links no libpython, and
that the Codon sources and the CPython binding generator (typesafe_carla.pycarla:
its runtime and pruned.json, which `import typesafe_carla.carla as carla` builds
from) are present. For the libcarla backend it also checks
that the license notices of the statically linked code ship next to the
library (LICENSE.CARLA, THIRD_PARTY_NOTICES). Used by the release workflow
before publishing.
"""

from __future__ import annotations

import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="libcarla")
    parser.add_argument("--carla-ref", default=None,
                        help="the CARLA ref the wheel records (its name, e.g. ue5-dev, when "
                             "built from a SHA resolved from it)")
    parser.add_argument("--carla-commit", default=None,
                        help="the CARLA commit SHA LibCarla must have been built from")
    args = parser.parse_args()

    from typesafe_carla import paths

    lib = paths.native_library()
    native = paths.native_info(lib)
    backend = native["backend"]
    ref = native["carla_git_ref"]
    commit = native["carla_git_commit"]
    print(f"library  {lib}")
    print(f"abi      {native['abi']}")
    print(f"backend  {backend}")
    print(f"libcarla {native['libcarla_version']} ({ref} {commit})")

    errors = []
    if backend != args.backend:
        errors.append(f"backend is {backend!r}, expected {args.backend!r}")
    if args.carla_ref is not None and ref != args.carla_ref:
        errors.append(f"CARLA ref is {ref!r}, expected {args.carla_ref!r}")
    if args.carla_commit is not None and commit != args.carla_commit:
        errors.append(f"CARLA commit is {commit!r}, expected {args.carla_commit!r}")
    info = paths.build_info()
    if info.get("carla_git_commit") != commit or info.get("carla_git_ref") != ref:
        errors.append(f"BUILD_INFO.json disagrees with the library: {info}")
    try:
        paths.codon_modules_dir()
    except paths.PathError as e:
        errors.append(str(e))
    from pathlib import Path

    import typesafe_carla

    package = Path(typesafe_carla.__file__).parent
    for data in ("pycarla/__init__.py", "pycarla/_runtime.py", "pycarla/pruned.json",
                 "carla/__init__.py", "carla_build.py"):
        if not (package / data).is_file():
            errors.append(f"{data} is missing from the package")
    if backend == "libcarla":
        # Expects an installed wheel: a source-tree build dir (paths prefers
        # build/ in a checkout) holds THIRD_PARTY_NOTICES but not LICENSE.CARLA.
        for notice in ("LICENSE.CARLA", "THIRD_PARTY_NOTICES"):
            path = lib.parent / notice
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f"{path} is missing or empty")
        notices = lib.parent / "THIRD_PARTY_NOTICES"
        if (notices.is_file() and commit not in ("", "unknown")
                and commit not in notices.read_text(errors="replace")):
            errors.append(f"{notices} does not name the CARLA commit {commit}")
    ldd = subprocess.run(["ldd", str(lib)], capture_output=True, text=True).stdout
    if "libpython" in ldd:
        errors.append("native library links libpython")
    exported = subprocess.run(["nm", "-D", "--defined-only", str(lib)],
                              capture_output=True, text=True).stdout.split("\n")
    stray = [line for line in exported if line.strip() and " tsc_" not in line]
    if stray:
        errors.append(f"unexpected exported symbols: {stray[:5]}")

    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
