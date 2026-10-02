"""Locates the files typesafe_carla ships: Codon sources and the native library.

Two layouts are supported:

* installed wheel::

      site-packages/typesafe_carla/_codon/typesafe_carla/__init__.codon
      site-packages/typesafe_carla/_native/libtypesafe_carla_ffi.so

* source checkout (development)::

      <repo>/codon/typesafe_carla/__init__.codon
      <repo>/build/libtypesafe_carla_ffi.so      (cmake -S . -B build)
"""

from __future__ import annotations

import os
from pathlib import Path

LIBRARY_NAME = "libtypesafe_carla_ffi.so"

ENV_LIB = "TYPESAFE_CARLA_LIB"
ENV_BUILD_DIR = "TYPESAFE_CARLA_BUILD_DIR"

_PACKAGE_DIR = Path(__file__).resolve().parent


class PathError(RuntimeError):
    pass


def _source_root() -> Path | None:
    """The repository root when running from a source checkout."""
    root = _PACKAGE_DIR.parent.parent
    if (root / "codon" / "typesafe_carla" / "__init__.codon").is_file():
        return root
    return None


def _installed_dirs(name: str) -> list[Path]:
    """Candidate data directories next to the installed package.

    For editable installs the Python sources stay in the checkout while
    CMake-installed data goes to site-packages, so also look at every
    ``typesafe_carla`` directory on the import path.
    """
    import sys

    dirs = [_PACKAGE_DIR / name]
    for entry in sys.path:
        candidate = Path(entry or ".") / "typesafe_carla" / name
        if candidate not in dirs:
            dirs.append(candidate)
    return dirs


def codon_modules_dir() -> Path:
    """Directory to put on CODON_PATH (it contains ``typesafe_carla/``)."""
    # A checkout's sources win over the copy an editable install made, so
    # edits take effect without reinstalling.
    root = _source_root()
    if root is not None:
        return root / "codon"
    for candidate in _installed_dirs("_codon"):
        if (candidate / "typesafe_carla" / "__init__.codon").is_file():
            return candidate
    raise PathError("typesafe_carla Codon sources not found; the installation is incomplete")


def native_library() -> Path:
    """Absolute path of libtypesafe_carla_ffi.so."""
    override = os.environ.get(ENV_LIB)
    if override:
        path = Path(override).expanduser().resolve()
        if not path.is_file():
            raise PathError(f"{ENV_LIB}={override} does not exist")
        return path
    # In a source checkout the developer's CMake build wins over whatever an
    # (editable) install compiled, so that rebuilding is enough to test changes.
    root = _source_root()
    if root is not None:
        build_dir = Path(os.environ.get(ENV_BUILD_DIR, root / "build"))
        if (build_dir / LIBRARY_NAME).is_file():
            return (build_dir / LIBRARY_NAME).resolve()
    for candidate in _installed_dirs("_native"):
        if (candidate / LIBRARY_NAME).is_file():
            return (candidate / LIBRARY_NAME).resolve()
    hint = "run `cmake -S . -B build && cmake --build build`" if root else "reinstall typesafe-carla"
    raise PathError(f"{LIBRARY_NAME} not found; {hint} or set {ENV_LIB}")
