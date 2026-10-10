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
import platform
import re
import subprocess
import sys

# Registers of optional vector extensions, as `objdump -d` prints them: code
# that uses them was compiled for the build machine's CPU (not portable,
# --disable-native) and dies (SIGILL) on a CPU without them.
NON_BASELINE = {
    "x86_64": ("AVX", re.compile(r"%[yz]mm\d")),
    "aarch64": ("SVE", re.compile(r"\b[zp]\d+\.[bhsdq]\b|\bp\d+/[zm]\b")),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="libcarla")
    parser.add_argument("--carla-ref", default=None,
                        help="the CARLA ref the wheel records (its name, e.g. ue5-dev, when "
                             "built from a SHA resolved from it)")
    parser.add_argument("--carla-commit", default=None,
                        help="the CARLA commit SHA LibCarla must have been built from")
    parser.add_argument("--pycarla", action="store_true",
                        help="the wheel must carry a prebuilt typesafe_carla.carla that matches "
                             "its sources and toolchain (tools/add_pycarla_to_wheel.py)")
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
    if args.pycarla:
        from typesafe_carla import carla_build

        if not carla_build.prebuilt_is_current():
            errors.append(f"no prebuilt typesafe_carla.carla matching this installation in "
                          f"{carla_build.PREBUILT_DIR}")
        else:
            so = carla_build.PREBUILT_DIR / "_carla.so"
            needed = subprocess.run(["readelf", "-d", str(so)], capture_output=True, text=True).stdout
            if "libpython" in needed:
                errors.append(f"{so} links libpython")
            # Built for any CPU of this architecture (NON_BASELINE).
            asm = subprocess.run(["objdump", "-d", str(so)], capture_output=True, text=True)
            extension, registers = NON_BASELINE.get(platform.machine(), (None, None))
            if asm.returncode != 0:
                errors.append(f"objdump -d {so} failed: {asm.stderr.strip()}")
            elif registers is None:
                errors.append(f"no portability check for {platform.machine()}")
            elif registers.search(asm.stdout):
                errors.append(f"{so} uses {extension} instructions: not built portable "
                              "(--disable-native)")
            print(f"pycarla  {carla_build.PREBUILT_DIR}")
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
