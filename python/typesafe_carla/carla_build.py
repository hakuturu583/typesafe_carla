"""Finds, and on first use builds, the CPython package `typesafe_carla.carla`.

`import typesafe_carla.carla as carla` gives CPython programs typesafe_carla with
the CARLA Python API's names (`carla.Client`, `carla.Transform`, ...). It is
the library compiled with `codon build --pyext` (typesafe_carla.pycarla).
That compile takes about 15 minutes and 8 GB of memory, needs a C compiler
(`cc`) to link, and depends on the Codon toolchain and the native library
this installation has, so it is not in the wheel: it is built once per
installation, into a cache, the first time it is imported, or ahead of time
with `typesafe-codon pycarla`.

Environment:
  TYPESAFE_CARLA_PYCARLA_DIR    the directory to build into and load from
                                (default: <cache>/typesafe_carla/pycarla/<key>,
                                <cache> being $XDG_CACHE_HOME or ~/.cache);
  TYPESAFE_CARLA_PYCARLA_BUILD  "0": never build on import; importing a missing
                                or stale build raises ImportError instead.

A build is keyed by what it is made from (the Codon sources, the generator,
the toolchain's version and the native library's path), so upgrading
typesafe-carla builds again, into a new directory. Concurrent first imports
(e.g. pytest-xdist workers) wait for one build: it holds a file lock.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

ENV_DIR = "TYPESAFE_CARLA_PYCARLA_DIR"
ENV_BUILD = "TYPESAFE_CARLA_PYCARLA_BUILD"


def cache_root() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base).expanduser() / "typesafe_carla" / "pycarla"


def build_dir() -> Path:
    """Where the package is (to be) built: TYPESAFE_CARLA_PYCARLA_DIR, else a
    directory of the cache named after the build's key."""
    explicit = os.environ.get(ENV_DIR)
    if explicit:
        return Path(explicit).expanduser().resolve()
    from typesafe_carla import __version__, pycarla

    stamp = pycarla.source_stamp(pycarla.LIBRARY_PACKAGE)
    return cache_root() / f"{__version__}-{stamp[:16]}"


def is_current(out: Path) -> bool:
    from typesafe_carla import pycarla

    return pycarla.is_current(out, pycarla.LIBRARY_PACKAGE)


def _stderr(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def ensure_built(log=_stderr) -> Path:
    """The build directory, holding a current build (`<dir>/carla`); builds it
    if it is missing or stale, unless TYPESAFE_CARLA_PYCARLA_BUILD=0."""
    out = build_dir()
    if is_current(out):
        return out
    if os.environ.get(ENV_BUILD, "1") == "0":
        raise ImportError(
            f"typesafe_carla.carla is not built in {out} and {ENV_BUILD}=0: "
            f"run `typesafe-codon pycarla` (~15 min, ~8 GB of memory)")
    import fcntl

    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out.parent / f".{out.name}.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not is_current(out):  # another process may have built it meanwhile
            _build(out, log)
    return out


def _build(out: Path, log) -> None:
    from typesafe_carla import pycarla

    log(f"typesafe_carla.carla: building the CPython package into {out} "
        f"(once per installation; ~15 min, ~8 GB of memory) ...")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # The shipped list of variants that do not compile, as a copy the build
    # may add to (the installed package can be read-only).
    pruned = out / "pruned.json"
    if pycarla.PRUNED.is_file():
        shutil.copyfile(pycarla.PRUNED, pruned)
    pycarla.build(out, package=pycarla.LIBRARY_PACKAGE, pruned_path=pruned, log=log)
    # Keyed by the shipped list, which is what is_current() compares.
    (out / pycarla.STAMP).write_text(pycarla.source_stamp(pycarla.LIBRARY_PACKAGE) + "\n")
    # Only the package and the stamp are loaded; the generated source, the
    # object file and the probe compiles are hundreds of MB (or a container
    # image layer) of nothing.
    keep = {"carla", pycarla.STAMP, "pruned.json"}
    for entry in out.iterdir():
        if entry.name not in keep:
            shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
    log(f"typesafe_carla.carla: built {out}")
