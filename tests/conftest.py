from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from typesafe_carla import paths, toolchain

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-9;]*m")


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
    """Runs `typesafe-codon <args>` and returns the CompletedProcess.

    ANSI colour codes are stripped from stdout and stderr.
    """
    if _SKIP_REASON:
        pytest.skip(_SKIP_REASON)

    def run(*args: str, timeout: float = 300, env: dict[str, str] | None = None):
        result = subprocess.run(
            [sys.executable, "-m", "typesafe_carla.cli", *args],
            capture_output=True, text=True, timeout=timeout, cwd=ROOT,
            env={**os.environ, **(env or {})})
        result.stdout = ANSI.sub("", result.stdout)
        result.stderr = ANSI.sub("", result.stderr)
        return result

    return run


@pytest.fixture(scope="session")
def backend() -> str:
    if _SKIP_REASON:
        pytest.skip(_SKIP_REASON)
    return paths.native_info()["backend"]


def pytest_terminal_summary(terminalreporter):
    """Per-file pass / xfail / skip counts of CARLA's own tests (test_upstream.py)."""
    lines = [value for reports in terminalreporter.stats.values() for r in reports
             for key, value in getattr(r, "user_properties", ()) if key == "upstream_counts"]
    if lines:
        terminalreporter.section("CARLA PythonAPI tests (tests/upstream)")
        for line in sorted(set(lines)):
            terminalreporter.write_line(line)
