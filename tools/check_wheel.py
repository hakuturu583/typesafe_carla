"""Sanity checks for a built typesafe-carla wheel, run against the installed package.

    python tools/check_wheel.py [--backend libcarla] [--carla-ref REF] [--carla-commit SHA]

Checks that the native library loads, exports the expected ABI, was built
with the expected backend (and CARLA ref and commit), links no libpython, and that the
Codon sources are present. Used by the release workflow before publishing.
"""

from __future__ import annotations

import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="libcarla")
    parser.add_argument("--carla-ref", default=None,
                        help="CARLA_GIT_REF the wheel was built from (a release passes the SHA)")
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
    if info.get("carla_git_commit") != commit:
        errors.append(f"BUILD_INFO.json disagrees with the library: {info}")
    try:
        paths.codon_modules_dir()
    except paths.PathError as e:
        errors.append(str(e))
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
