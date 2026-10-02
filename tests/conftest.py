from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from typesafe_carla import paths, toolchain

ROOT = Path(__file__).resolve().parent.parent


def _codon_available() -> str | None:
    try:
        toolchain.find_codon()
        paths.native_library()
    except (toolchain.ToolchainError, paths.PathError) as e:
        return str(e)
    return None


_SKIP_REASON = _codon_available()


@pytest.fixture(scope="session")
def launcher():
    """Runs `typesafe-codon <args>` and returns the CompletedProcess."""
    if _SKIP_REASON:
        pytest.skip(_SKIP_REASON)

    def run(*args: str, timeout: float = 300, env: dict[str, str] | None = None):
        return subprocess.run(
            [sys.executable, "-m", "typesafe_carla.cli", *args],
            capture_output=True, text=True, timeout=timeout, cwd=ROOT,
            env={**os.environ, **(env or {})})

    return run


@pytest.fixture(scope="session")
def backend() -> str:
    import ctypes

    if _SKIP_REASON:
        pytest.skip(_SKIP_REASON)
    so = ctypes.CDLL(str(paths.native_library()))
    so.tsc_backend_name.restype = ctypes.c_char_p
    return so.tsc_backend_name().decode()
