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
release -- the package is built instead, into a cache, on first import or
ahead of time with `typesafe-codon pycarla`. That compile takes 15 to 50
minutes and about 14 GB of memory and needs a C compiler (`cc`) to link. When
the wheel's prebuilt package is there but does not match, the import says why
first (explain_prebuilt_mismatch(): which installed file differs from what the
wheel installed, or which toolchain release it was built with).

A cache build is, like the prebuilt package, independent of the installation
that made it: it names no native library and no Codon runtime directory.
prepare_runtime() gives it the importing installation's native library
(TYPESAFE_CARLA_LIB) and Codon runtime, and the library's own ABI check
rejects a native library it does not fit. So it is keyed by what it is
compiled from alone (cache_key()), and every installation (virtual
environment) with the same sources and Codon shares one build.

Environment:
  TYPESAFE_CARLA_PYCARLA_DIR    the directory to build into and load from
                                (default: <cache>/typesafe_carla/pycarla/<key>,
                                <cache> being $XDG_CACHE_HOME or ~/.cache);
  TYPESAFE_CARLA_PYCARLA_BUILD  "0": never build on import; importing a missing
                                or stale build raises ImportError instead.

TYPESAFE_CARLA_PYCARLA_DIR wins over the prebuilt package. A build is keyed
by what it is made from (the Codon sources, the generator and the Codon
toolchain), so upgrading typesafe-carla builds again, into a new directory.
Concurrent first imports (e.g. pytest-xdist workers, or several virtual
environments) wait for one build: it holds a file lock.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

ENV_DIR = "TYPESAFE_CARLA_PYCARLA_DIR"
ENV_BUILD = "TYPESAFE_CARLA_PYCARLA_BUILD"

#: The wheel's prebuilt package: `_carla.so`, `_spec.json`, `_runtime.py`, a
#: stamp (prebuilt_stamp()) and the toolchain release it was built with
#: (TOOLCHAIN_FILE; wheels after 0.3.0, for explain_prebuilt_mismatch()).
PREBUILT_DIR = Path(__file__).resolve().parent / "carla" / "_prebuilt"
#: From PREBUILT_DIR/_carla.so to the Codon runtime of an installed
#: typesafe-carla-toolchain (site-packages/typesafe_carla_toolchain/codon/
#: lib/codon). Also preloaded by prepare_runtime(), which covers any layout.
PREBUILT_RPATH = "$ORIGIN/../../../typesafe_carla_toolchain/codon/lib/codon"
#: In a prebuilt package: the typesafe-carla-toolchain version it was built with.
TOOLCHAIN_FILE = "toolchain"
DISTRIBUTION = "typesafe-carla"
TOOLCHAIN_DISTRIBUTION = "typesafe-carla-toolchain"


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


def _toolchain_version() -> str | None:
    from importlib import metadata

    try:
        return metadata.version(TOOLCHAIN_DISTRIBUTION)
    except metadata.PackageNotFoundError:
        return None


def _wheel_record():
    """The installed typesafe-carla distribution PREBUILT_DIR came with, and
    its RECORD: {installed path: (RECORD path, "sha256=<digest>")}. None for a
    checkout, or an installation without a RECORD. (Read directly: since
    Python 3.12, Distribution.files leaves out the files that are missing.)"""
    import csv
    from importlib import metadata

    stamp = (PREBUILT_DIR / "stamp").resolve()
    for dist in metadata.distributions(name=DISTRIBUTION):
        text = dist.read_text("RECORD")
        if not text:
            continue
        record = {}
        for row in csv.reader(text.splitlines()):
            if len(row) >= 2 and row[1]:
                record[Path(dist.locate_file(row[0])).resolve()] = (row[0], row[1])
        if stamp in record:
            return dist, record
    return None


def _record_hash(path: Path) -> str:
    """`path`'s sha256 as a wheel's RECORD writes it."""
    import base64
    import hashlib

    digest = hashlib.sha256(path.read_bytes()).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode()


def explain_prebuilt_mismatch() -> list[str]:
    """Why the wheel's prebuilt package does not match this installation
    (prebuilt_stamp() covers the installed Codon sources, the generator, its
    runtime, pruned.json and the toolchain release): each installed file the
    stamp covers that differs from what the wheel installed (its RECORD), and
    a toolchain release other than the one the package was built with. Empty
    if there is no prebuilt package or it matches. Reads a few MB: called only
    when the stamps differ."""
    if prebuilt_is_current() or not (PREBUILT_DIR / "stamp").is_file():
        return []
    from typesafe_carla import pycarla

    reasons = []
    built_with = None
    if (PREBUILT_DIR / TOOLCHAIN_FILE).is_file():
        built_with = (PREBUILT_DIR / TOOLCHAIN_FILE).read_text().strip()
    installed = _toolchain_version()
    if built_with is not None and built_with != (installed or ""):
        reasons.append(
            f"the prebuilt package was built with {TOOLCHAIN_DISTRIBUTION} {built_with}, and "
            f"{installed or 'none'} is installed: install {TOOLCHAIN_DISTRIBUTION}=={built_with} "
            f"to use it")
    if os.environ.get("PYCARLA_PRUNED"):
        reasons.append(f"PYCARLA_PRUNED={os.environ['PYCARLA_PRUNED']} replaces the shipped "
                       f"pruned.json")
    found = _wheel_record()
    if found is None:
        reasons.append(f"no installed {DISTRIBUTION} distribution lists {PREBUILT_DIR} in its "
                       f"RECORD, so the installed files cannot be checked against the wheel")
        return reasons
    dist, record = found
    covered = sorted(pycarla.PACKAGE.glob("*.codon")) + [Path(pycarla.__file__), pycarla.RUNTIME,
                                                         pycarla.PRUNED]
    version = dist.version
    modified = []
    for path in covered:
        path = path.resolve()
        entry = record.get(path)
        if entry is None:
            if path.is_file() and not (path == pycarla.PRUNED.resolve()
                                       and os.environ.get("PYCARLA_PRUNED")):
                reasons.append(f"{path} is not a file the {DISTRIBUTION} {version} wheel "
                               f"installed (a source checkout's or an editable install's?)")
        elif not path.is_file() or f"sha256={_record_hash(path)}" != entry[1]:
            modified.append(entry[0])
    codon_dir = pycarla.PACKAGE.resolve()
    for path, entry in record.items():
        if path.parent == codon_dir and path.suffix == ".codon" and not path.is_file():
            modified.append(f"{entry[0]} (missing)")
    if modified:
        reasons.append(
            f"{', '.join(modified)} differ{'s' if len(modified) == 1 else ''} from the "
            f"{DISTRIBUTION} {version} wheel: the installation was modified (by hand, or "
            f"through a package cache that hardlinks installed files, e.g. uv's). Reinstall it "
            f"from a clean copy, e.g. `uv cache clean {DISTRIBUTION}` and then "
            f"`uv sync --reinstall-package {DISTRIBUTION}`")
    if not reasons:
        reasons.append(
            f"the installed files are the {DISTRIBUTION} {version} wheel's, so the "
            f"{TOOLCHAIN_DISTRIBUTION} release ({installed or 'none installed'}) is probably not "
            f"the one the prebuilt package was built with")
    return reasons


def _prebuilt_mismatch_message() -> str:
    """Why the wheel's prebuilt package is not used ("" if it is not there, it
    matches, or TYPESAFE_CARLA_PYCARLA_DIR overrides it)."""
    if os.environ.get(ENV_DIR):
        return ""
    reasons = explain_prebuilt_mismatch()
    if not reasons:
        return ""
    return ("typesafe_carla.carla: the wheel's prebuilt package does not match this "
            "installation and is not used:\n" + "\n".join(f"  - {r}" for r in reasons))


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


def _codon_identity() -> bytes:
    """What prebuilt_stamp() does not cover about the Codon that compiles and
    runs the package: nothing for the bundled toolchain (its release is in
    the stamp), else (TYPESAFE_CODON, CODON_DIR, ~/.codon, PATH) the Codon
    runtime library's bytes and the compiler's version."""
    import hashlib

    from typesafe_carla import toolchain

    try:
        tc = toolchain.find_codon()
    except toolchain.ToolchainError:
        return b"no codon"
    if tc.source == "typesafe-carla-toolchain":
        return b""
    h = hashlib.sha256(tc.version().encode() + b"\0")
    for directory in tc.library_dirs():
        runtime = directory / "libcodonrt.so"
        if runtime.is_file():
            with open(runtime, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
            break
    return h.hexdigest().encode()


def cache_key() -> str:
    """The key of a build of the cache (or of TYPESAFE_CARLA_PYCARLA_DIR):
    the prebuilt package's key, this machine's architecture (a home
    directory shared with machines of another one, e.g. x86_64 and aarch64)
    and the identity of a Codon other than the bundled toolchain. Not the
    native library or any path of this installation: the build finds both at
    run time (prepare_runtime()), so one build serves every installation with
    the same sources, Codon and architecture. (The prebuilt package needs no
    architecture: a wheel is for one.)"""
    import hashlib
    import platform

    h = hashlib.sha256(prebuilt_stamp().encode() + b"\0machine\0" + platform.machine().encode())
    codon = _codon_identity()
    if codon:
        h.update(b"\0codon\0" + codon)
    return h.hexdigest()


def build_dir() -> Path:
    """Where the package is (to be) built: TYPESAFE_CARLA_PYCARLA_DIR, else a
    directory of the cache named after the build's key."""
    explicit = os.environ.get(ENV_DIR)
    if explicit:
        return Path(explicit).expanduser().resolve()
    from typesafe_carla import __version__

    return cache_root() / f"{__version__}-{cache_key()[:16]}"


def is_current(out: Path) -> bool:
    from typesafe_carla import pycarla

    stamp = out / pycarla.STAMP
    return (out / "carla" / f"{pycarla.MODULE}.so").is_file() and stamp.is_file() \
        and stamp.read_text().strip() == cache_key()


def finish_build(out: Path) -> None:
    """Makes `out`, built by pycarla.build(package=LIBRARY_PACKAGE, rpath=[]),
    a build is_current() accepts: keyed by cache_key() (the shipped
    pruned.json, as is_current() compares) and naming no native library."""
    from typesafe_carla import pycarla

    _forget_native_library(out / "carla")
    (out / pycarla.STAMP).write_text(cache_key() + "\n")


def _forget_native_library(pkg: Path) -> None:
    """Drops the builder's native library from a build's `_spec.json`: a
    build of typesafe_carla.carla always loads the importing installation's
    (prepare_runtime()), never the one of the installation that built it."""
    import json

    spec_path = pkg / "_spec.json"
    spec = json.loads(spec_path.read_text())
    spec.pop("native_library", None)
    spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True))


def _stderr(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def make_prebuilt(dest: Path, log=_stderr) -> Path:
    """Builds the package for a wheel into `dest` (tools/add_pycarla_to_wheel.py):
    keyed by prebuilt_stamp() (with the toolchain release beside it, for
    explain_prebuilt_mismatch()), linked to find the Codon runtime relative
    to itself, and compiled for any CPU of the architecture (not the build machine's). Run
    it with the typesafe-carla and toolchain the wheel is installed with."""
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
        _forget_native_library(out / "carla")
        for name in (f"{pycarla.MODULE}.so", "_spec.json", "_runtime.py"):
            shutil.copyfile(out / "carla" / name, dest / name)
    (dest / pycarla.STAMP).write_text(prebuilt_stamp() + "\n")
    (dest / TOOLCHAIN_FILE).write_text((_toolchain_version() or "") + "\n")
    return dest


def ensure_built(log=_stderr) -> Path:
    """The build directory, holding a current build (`<dir>/carla`); builds it
    if it is missing or stale, unless TYPESAFE_CARLA_PYCARLA_BUILD=0."""
    out = build_dir()
    if is_current(out):
        return out
    # About to build (or to refuse to): say why the wheel's build does not do.
    mismatch = _prebuilt_mismatch_message()
    if os.environ.get(ENV_BUILD, "1") == "0":
        raise ImportError(
            f"typesafe_carla.carla is not built in {out} and {ENV_BUILD}=0: "
            f"run `typesafe-codon pycarla` (15-50 min, ~14 GB of memory)"
            + (f"\n{mismatch}" if mismatch else ""))
    if mismatch:
        log(mismatch)
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
        f"(once for these sources and Codon, shared by every installation; "
        f"15-50 min, ~14 GB of memory) ...")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # The shipped list of variants that do not compile, as a copy the build
    # may add to (the installed package can be read-only).
    pruned = out / "pruned.json"
    if pycarla.PRUNED.is_file():
        shutil.copyfile(pycarla.PRUNED, pruned)
    # No RPATH: like the prebuilt package, the build is shared by every
    # installation, and prepare_runtime() loads the importing one's Codon
    # runtime (and names its native library) before `_carla.so` is loaded.
    pycarla.build(out, package=pycarla.LIBRARY_PACKAGE, pruned_path=pruned, log=log, rpath=[])
    finish_build(out)
    # Only the package and the stamp are loaded; the generated source, the
    # object file and the probe compiles are hundreds of MB (or a container
    # image layer) of nothing.
    keep = {"carla", pycarla.STAMP, "pruned.json"}
    for entry in out.iterdir():
        if entry.name not in keep:
            shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
    log(f"typesafe_carla.carla: built {out}")
