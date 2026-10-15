"""The LibCarla patches (cmake/carla-patches/README.md) and their patch id.

The patch id is the first 12 hex digits of the sha256 of
"<file name> <sha256 of the file>\\n" for each cmake/carla-patches/*.patch in
file-name order, or "none" if there is none: the same as
`tsc_carla_patch_id` in cmake/CarlaPatches.cmake, which must agree with it
(tests/test_carla_patches.py checks both).
"""

from __future__ import annotations

import hashlib
from pathlib import Path

PATCH_DIR = Path(__file__).resolve().parent.parent / "cmake" / "carla-patches"
NONE = "none"


def patch_files(directory: Path = PATCH_DIR) -> list[Path]:
    """The patches, in the order they are applied."""
    if not directory.is_dir():
        return []
    # Sorted by name as UTF-8 bytes, as CMake's list(SORT) does.
    return sorted((p for p in directory.glob("*.patch") if p.is_file()),
                  key=lambda p: p.name.encode())


def patch_id(directory: Path = PATCH_DIR) -> str:
    files = patch_files(directory)
    if not files:
        return NONE
    text = "".join(f"{p.name} {hashlib.sha256(p.read_bytes()).hexdigest()}\n" for p in files)
    return hashlib.sha256(text.encode()).hexdigest()[:12]


if __name__ == "__main__":
    print(patch_id())
