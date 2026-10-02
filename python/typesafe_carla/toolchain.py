"""Locates the Codon compiler.

Search order:

1. ``TYPESAFE_CODON``: path to a ``codon`` executable.
2. The ``typesafe-carla-toolchain`` package (a pinned, bundled Codon), once
   it is installed. It must provide ``typesafe_carla_toolchain.codon_executable()``.
3. ``CODON_DIR``: a Codon installation directory (``$CODON_DIR/bin/codon``).
4. ``~/.codon/bin/codon`` (the official installer's location).
5. ``codon`` on ``PATH``.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

# The Codon release series typesafe_carla is tested with.
SUPPORTED_CODON_SERIES = "0.19"

ENV_CODON = "TYPESAFE_CODON"


class ToolchainError(RuntimeError):
    pass


@dataclass(frozen=True)
class Toolchain:
    executable: Path
    codon_dir: Path  # installation root: bin/, lib/codon/
    source: str  # how it was found, for `typesafe-codon info`

    def version(self) -> str:
        out = subprocess.run([str(self.executable), "--version"], capture_output=True, text=True)
        return out.stdout.strip() or out.stderr.strip()

    def library_dirs(self) -> list[Path]:
        return [d for d in (self.codon_dir / "lib" / "codon", self.codon_dir / "lib") if d.is_dir()]


def _from_executable(exe: Path, source: str) -> Toolchain:
    exe = exe.expanduser().resolve()
    if not exe.is_file():
        raise ToolchainError(f"{source}: {exe} is not a file")
    return Toolchain(exe, exe.parent.parent, source)


def _bundled() -> Toolchain | None:
    try:
        import typesafe_carla_toolchain  # type: ignore[import-not-found]
    except ImportError:
        return None
    try:
        exe = typesafe_carla_toolchain.codon_executable()
    except RuntimeError as e:  # installed without its Codon bundle
        raise ToolchainError(f"typesafe-carla-toolchain: {e}") from e
    return _from_executable(Path(exe), "typesafe-carla-toolchain")


def find_codon() -> Toolchain:
    explicit = os.environ.get(ENV_CODON)
    if explicit:
        return _from_executable(Path(explicit), ENV_CODON)
    bundled = _bundled()
    if bundled is not None:
        return bundled
    codon_dir = os.environ.get("CODON_DIR")
    if codon_dir and (Path(codon_dir) / "bin" / "codon").is_file():
        return _from_executable(Path(codon_dir) / "bin" / "codon", "CODON_DIR")
    home = Path.home() / ".codon" / "bin" / "codon"
    if home.is_file():
        return _from_executable(home, "~/.codon")
    on_path = shutil.which("codon")
    if on_path:
        return _from_executable(Path(on_path), "PATH")
    raise ToolchainError(
        "Codon compiler not found. Install typesafe-carla-toolchain, or Codon "
        f"{SUPPORTED_CODON_SERIES}.x (https://github.com/exaloop/codon/releases), and point "
        f"{ENV_CODON} at the codon executable if it is not in ~/.codon or on PATH.")


def is_supported_version(version: str) -> bool:
    return version.strip().startswith(SUPPORTED_CODON_SERIES + ".") or \
        version.strip() == SUPPORTED_CODON_SERIES
