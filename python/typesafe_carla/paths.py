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
        explicit = os.environ.get(ENV_BUILD_DIR)
        build_dir = Path(explicit).expanduser().resolve() if explicit else root / "build"
        if (build_dir / LIBRARY_NAME).is_file():
            return (build_dir / LIBRARY_NAME).resolve()
        if explicit:
            raise PathError(f"{ENV_BUILD_DIR}={explicit}: no {LIBRARY_NAME} in {build_dir}")
    for candidate in _installed_dirs("_native"):
        if (candidate / LIBRARY_NAME).is_file():
            return (candidate / LIBRARY_NAME).resolve()
    hint = "run `cmake -S . -B build && cmake --build build`" if root else "reinstall typesafe-carla"
    raise PathError(f"{LIBRARY_NAME} not found; {hint} or set {ENV_LIB}")


def build_info() -> dict[str, str]:
    """Contents of BUILD_INFO.json next to the native library (empty if absent).

    Keys: typesafe_carla, build_commit, backend, carla_version, carla_git_ref,
    carla_git_commit.
    """
    import json

    info = native_library().parent / "BUILD_INFO.json"
    if not info.is_file():
        return {}
    return json.loads(info.read_text())


_NATIVE_STRINGS = {
    "backend": "tsc_backend_name",
    "libcarla_version": "tsc_libcarla_version",
    "carla_git_ref": "tsc_libcarla_git_ref",
    "carla_git_commit": "tsc_libcarla_git_commit",
    "build_commit": "tsc_build_commit",
}


def native_info(lib: Path | None = None) -> dict[str, str]:
    """Version strings reported by the loaded native library.

    Keys: abi ("major.minor") plus those of ``_NATIVE_STRINGS``; getters an
    older ABI lacks read "n/a (older ABI)". Raises OSError if it cannot load.
    """
    import ctypes

    so = ctypes.CDLL(str(lib or native_library()))
    abi = so.tsc_abi_version()
    info = {"abi": f"{abi >> 16}.{abi & 0xFFFF}"}
    for key, name in _NATIVE_STRINGS.items():
        fn = getattr(so, name, None)
        if fn is None:
            info[key] = "n/a (older ABI)"
        else:
            fn.restype = ctypes.c_char_p
            info[key] = fn().decode()
    return info


ENV_CACHE_DIR = "TYPESAFE_CARLA_CACHE_DIR"
BUILD_CONFIG_MODULE = "_tsc_build_config"


def _cache_root() -> Path:
    explicit = os.environ.get(ENV_CACHE_DIR)
    if explicit:
        return Path(explicit).expanduser()
    xdg = os.environ.get("XDG_CACHE_HOME")
    return (Path(xdg) if xdg else Path.home() / ".cache") / "typesafe-carla"


def _private_tmp_root() -> Path | None:
    """``$TMPDIR/typesafe-carla-<uid>``, if it is (or can be made) a directory
    private to this user: not a symlink, owned by us, not group/other writable.
    """
    import stat
    import tempfile

    root = Path(tempfile.gettempdir()) / f"typesafe-carla-{os.getuid()}"
    try:
        root.mkdir(mode=0o700, exist_ok=True)
        st = os.lstat(root)
    except OSError:
        return None
    if (not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid()
            or st.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
        return None
    return root


def _cache_roots():
    """Candidate cache roots, best first: $TYPESAFE_CARLA_CACHE_DIR, else
    ~/.cache/typesafe-carla (or $XDG_CACHE_HOME), then a private temp directory."""
    yield _cache_root()
    if not os.environ.get(ENV_CACHE_DIR):
        tmp = _private_tmp_root()
        if tmp is not None:
            yield tmp


def _build_config_source(strict: bool) -> str:
    return ("# Generated by typesafe-codon; imported by typesafe_carla/_strict.codon.\n"
            f"TSC_STRICT: Static[int] = {int(strict)}\n")


def _is_codon_path_dir(directory: Path, package: Path, config: str) -> bool:
    try:
        return ((directory / "typesafe_carla").resolve() == package
                and (directory / f"{BUILD_CONFIG_MODULE}.codon").read_text() == config)
    except OSError:
        return False


def _make_codon_path_dir(base: Path, name: str, package: Path, config: str) -> Path:
    import shutil
    import tempfile

    target = base / name
    if _is_codon_path_dir(target, package, config):
        return target
    base.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f".{name}.", dir=base))
    try:
        (tmp / "typesafe_carla").symlink_to(package, target_is_directory=True)
        (tmp / f"{BUILD_CONFIG_MODULE}.codon").write_text(config)
        # Another process may have installed a valid directory meanwhile: keep it.
        # Only a directory whose content differs (other sources/config) is stale.
        if _is_codon_path_dir(target, package, config):
            return target
        if target.exists() or target.is_symlink():
            stale = Path(tempfile.mkdtemp(prefix=f".{name}.stale.", dir=base))
            try:
                os.rename(target, stale / name)
            except OSError:
                pass  # another process replaced it first
            shutil.rmtree(stale, ignore_errors=True)
        try:
            os.rename(tmp, target)  # atomic; fails if another process created it first
        except OSError:
            if not _is_codon_path_dir(target, package, config):
                raise
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return target


def codon_path_dir(strict: bool) -> Path:
    """The directory to use as CODON_PATH: ``typesafe_carla/`` plus the build config.

    Codon's ``-D`` defines are only visible in the main program file, so the
    compile-time switches the library reads (strict mode) come from a generated
    ``_tsc_build_config.codon`` next to a symlink to the Codon sources. One
    directory per (sources, strict) pair is cached under ``codon-path/`` of the
    first usable cache root (see ``_cache_roots``) and created atomically, so
    concurrent runs are safe. Only compilation reads it: executables built from
    it do not depend on it.
    """
    import hashlib

    package = (codon_modules_dir() / "typesafe_carla").resolve()
    config = _build_config_source(strict)
    name = f"{hashlib.sha256(str(package).encode()).hexdigest()[:16]}-strict{int(strict)}"
    error: OSError | None = None
    for root in _cache_roots():
        try:
            return _make_codon_path_dir(root / "codon-path", name, package, config)
        except OSError as e:
            error = e
    raise PathError(f"cannot create the Codon module directory under {_cache_root()}: {error}; "
                    f"set {ENV_CACHE_DIR} to a writable directory")


SCRATCH_MAX_AGE = 24 * 3600  # seconds


def _sweep_scratch(run: Path) -> None:
    """Removes scratch directories older than a day (left by a killed launcher)."""
    import shutil
    import time

    cutoff = time.time() - SCRATCH_MAX_AGE
    for entry in run.glob("typesafe-codon-*"):
        try:
            if entry.lstat().st_mtime < cutoff:
                shutil.rmtree(entry, ignore_errors=True)
        except OSError:
            pass


def scratch_dir() -> str:
    """A new private directory for the launcher's temporary files (caller removes it).

    Under ``run/`` of the cache root rather than /tmp, which may be mounted
    noexec (the launcher executes programs from here); the system temp
    directory is the last resort.
    """
    import tempfile

    for root in _cache_roots():
        try:
            (root / "run").mkdir(parents=True, exist_ok=True)
            _sweep_scratch(root / "run")
            return tempfile.mkdtemp(prefix="typesafe-codon-", dir=root / "run")
        except OSError:
            continue
    return tempfile.mkdtemp(prefix="typesafe-codon-")
