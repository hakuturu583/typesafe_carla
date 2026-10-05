"""Finds, or builds, the CPython package `typesafe_carla.carla`.

`import typesafe_carla.carla as carla` gives CPython programs typesafe_carla with
the CARLA Python API's names (`carla.Client`, `carla.Transform`, ...). It is
the library compiled with `codon build --pyext` (typesafe_carla.pycarla).

The released wheel carries that build (`typesafe_carla/carla/_prebuilt`,
added by the release workflow with tools/add_pycarla_to_wheel.py): a
`pip install typesafe-carla` imports it with nothing to compile. Codon's
output does not depend on the Python version, and the build finds the native
library (TYPESAFE_CARLA_LIB) and the Codon runtime at run time, so one build
serves every installation of the same typesafe-carla and toolchain release.

Where there is no matching prebuilt package -- a source checkout or an
sdist install, edited Codon sources, another typesafe-carla-toolchain
release -- the package is built once per installation instead, into a cache,
on first import or ahead of time with `typesafe-codon pycarla`. That compile
takes 15 to 50 minutes and about 14 GB of memory and needs a C compiler
(`cc`) to link.

Environment:
  TYPESAFE_CARLA_PYCARLA_DIR    the directory to build into and load from
                                (default: <cache>/typesafe_carla/pycarla/<key>,
                                <cache> being $XDG_CACHE_HOME or ~/.cache);
  TYPESAFE_CARLA_PYCARLA_BUILD  "0": never build on import; importing a missing
                                or stale build raises ImportError instead.

TYPESAFE_CARLA_PYCARLA_DIR wins over the prebuilt package. A build is keyed
by what it is made from (the Codon sources, the generator, the toolchain's
version and, for a cache build, the native library's path), so upgrading
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

#: The wheel's prebuilt package: `_carla.so`, `_spec.json`, `_runtime.py` and
#: a stamp (prebuilt_stamp()).
PREBUILT_DIR = Path(__file__).resolve().parent / "carla" / "_prebuilt"
#: From PREBUILT_DIR/_carla.so to the Codon runtime of an installed
#: typesafe-carla-toolchain (site-packages/typesafe_carla_toolchain/codon/
#: lib/codon). Also preloaded by prepare_runtime(), which covers any layout.
PREBUILT_RPATH = "$ORIGIN/../../../typesafe_carla_toolchain/codon/lib/codon"


def prebuilt_stamp() -> str:
    """The key a prebuilt package must carry to be loaded: these Codon
    sources, generator and runtime, and this toolchain release."""
    from typesafe_carla import pycarla

    return pycarla.source_stamp(pycarla.LIBRARY_PACKAGE, native=False)


def prebuilt_is_current(directory: Path | None = None) -> bool:
    from typesafe_carla import pycarla

    directory = PREBUILT_DIR if directory is None else directory
    stamp = directory / pycarla.STAMP
    return (directory / f"{pycarla.MODULE}.so").is_file() and stamp.is_file() \
        and stamp.read_text().strip() == prebuilt_stamp()


def package_dir() -> Path:
    """The directory `typesafe_carla.carla` loads its generated files from:
    TYPESAFE_CARLA_PYCARLA_DIR's build, else the wheel's prebuilt package if
    it matches this installation, else the cache's build (built if needed)."""
    if not os.environ.get(ENV_DIR) and prebuilt_is_current():
        return PREBUILT_DIR
    return ensure_built() / "carla"


def prepare_runtime() -> None:
    """What the package needs before `_carla.so` is loaded: the native library
    of this installation (unless TYPESAFE_CARLA_LIB names one; the build's
    own `_spec.json` records the builder's) and the Codon runtime, loaded
    globally so `_carla.so` resolves it wherever the toolchain is installed."""
    import ctypes

    from typesafe_carla import paths, toolchain

    if not os.environ.get(paths.ENV_LIB):
        os.environ[paths.ENV_LIB] = str(paths.native_library())
    for directory in toolchain.find_codon().library_dirs():
        runtime = directory / "libcodonrt.so"
        if runtime.is_file():
            ctypes.CDLL(str(runtime), mode=ctypes.RTLD_GLOBAL)
            return


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


def make_prebuilt(dest: Path, log=_stderr) -> Path:
    """Builds the package for a wheel into `dest` (tools/add_pycarla_to_wheel.py):
    keyed by prebuilt_stamp(), linked to find the Codon runtime relative to
    itself, and compiled for any x86-64 CPU (not the build machine's). Run it with the typesafe-carla and toolchain the wheel is
    installed with."""
    import tempfile

    from typesafe_carla import pycarla

    with tempfile.TemporaryDirectory(prefix="pycarla-prebuilt-") as tmp:
        out = Path(tmp)
        pruned = out / "pruned.json"
        if pycarla.PRUNED.is_file():
            shutil.copyfile(pycarla.PRUNED, pruned)
        log(f"typesafe_carla.carla: building the prebuilt package (15-50 min) in {out} ...")
        pycarla.build(out, package=pycarla.LIBRARY_PACKAGE, pruned_path=pruned, log=log,
                      rpath=[PREBUILT_RPATH], portable=True)
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        for name in (f"{pycarla.MODULE}.so", "_spec.json", "_runtime.py"):
            shutil.copyfile(out / "carla" / name, dest / name)
    (dest / pycarla.STAMP).write_text(prebuilt_stamp() + "\n")
    return dest


def ensure_built(log=_stderr) -> Path:
    """The build directory, holding a current build (`<dir>/carla`); builds it
    if it is missing or stale, unless TYPESAFE_CARLA_PYCARLA_BUILD=0."""
    out = build_dir()
    if is_current(out):
        return out
    if os.environ.get(ENV_BUILD, "1") == "0":
        raise ImportError(
            f"typesafe_carla.carla is not built in {out} and {ENV_BUILD}=0: "
            f"run `typesafe-codon pycarla` (15-50 min, ~14 GB of memory)")
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
        f"(once per installation; 15-50 min, ~14 GB of memory) ...")
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
