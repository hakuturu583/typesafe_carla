"""Merges typesafe-carla wheels built for different CARLA refs into one.

    python tools/merge_native_wheels.py <base wheel> <wheel of another ref>...

A release builds one wheel per CARLA ref of `tools/release_carla_refs.txt`
(LibCarla, and so the native library, differs per ref; everything else is the
same). This adds the native library directory (`typesafe_carla/_native/`:
the library, BUILD_INFO.json, the license notices) of every other wheel to the
base wheel as `typesafe_carla/_native/refs/<CARLA ref>/`, RECORD updated, in
place. The base wheel's own ref stays the default; `TYPESAFE_CARLA_CARLA_REF`
selects another at run time (typesafe_carla.paths).

The wheels must be the same version and platform and hold the same files
outside `_native/`: only the native library may differ between refs. The ref
of each wheel is the `carla_git_ref` of its BUILD_INFO.json; refs must be
distinct and usable as a directory name.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

try:  # imported as tools.merge_native_wheels (tests) or run as a script
    from tools.add_pycarla_to_wheel import replace_dir
except ImportError:
    from add_pycarla_to_wheel import replace_dir

NATIVE = "typesafe_carla/_native"
REFS = f"{NATIVE}/refs"
_REF_DIR = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


class MergeError(ValueError):
    pass


def _members(wheel: zipfile.ZipFile) -> dict[str, bytes]:
    """Every file but RECORD (which differs whenever any file does)."""
    return {n: wheel.read(n) for n in wheel.namelist()
            if not n.endswith("/") and not n.endswith(".dist-info/RECORD")}


def _ref_of(name: str, files: dict[str, bytes]) -> str:
    try:
        ref = json.loads(files[f"{NATIVE}/BUILD_INFO.json"])["carla_git_ref"]
    except (KeyError, ValueError) as e:
        raise MergeError(f"{name}: no CARLA ref in {NATIVE}/BUILD_INFO.json ({e!r})") from None
    if not isinstance(ref, str) or not _REF_DIR.fullmatch(ref) or ref == "refs":
        raise MergeError(f"{name}: CARLA ref {ref!r} cannot name a directory under {REFS}")
    return ref


def merge(base: Path, others: list[Path]) -> list[str]:
    """Adds the native libraries of `others` to `base`; returns the refs, base first."""
    with zipfile.ZipFile(base) as z:
        base_files = _members(z)
    if any(n.startswith(REFS + "/") for n in base_files):
        raise MergeError(f"{base.name} already has {REFS}/")
    refs = [_ref_of(base.name, base_files)]
    shared = {n: d for n, d in base_files.items()
              if not n.startswith(NATIVE + "/") and ".dist-info/" not in n}
    added: dict[str, bytes] = {}
    for other in others:
        if other.name != base.name:
            raise MergeError(f"{other.name} is not the same version and platform as {base.name}")
        with zipfile.ZipFile(other) as z:
            files = _members(z)
        ref = _ref_of(str(other), files)
        if ref in refs:
            raise MergeError(f"{other}: CARLA ref {ref} is already in the wheel")
        theirs = {n: d for n, d in files.items()
                  if not n.startswith(NATIVE + "/") and ".dist-info/" not in n}
        differ = sorted(n for n in shared.keys() | theirs.keys() if shared.get(n) != theirs.get(n))
        if differ:
            raise MergeError(f"{other} ({ref}) differs from {base.name} outside {NATIVE}/: "
                             f"{', '.join(differ[:5])}")
        native = {n[len(NATIVE) + 1:]: d for n, d in files.items() if n.startswith(NATIVE + "/")}
        if "libtypesafe_carla_ffi.so" not in native:
            raise MergeError(f"{other} has no {NATIVE}/libtypesafe_carla_ffi.so")
        added.update({f"{ref}/{n}": d for n, d in native.items()})
        refs.append(ref)
    replace_dir(base, REFS, added)
    return refs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("base", type=Path, help="the wheel of the default CARLA ref")
    parser.add_argument("others", type=Path, nargs="*", help="wheels of the other CARLA refs")
    args = parser.parse_args(argv)
    try:
        refs = merge(args.base, args.others)
    except MergeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    print(f"{args.base}: CARLA {refs[0]} (default)"
          + "".join(f", {r} ({REFS}/{r})" for r in refs[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
