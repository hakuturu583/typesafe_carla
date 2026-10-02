"""Runtime tests: Codon programs in tests/unit, run against the mock backend.

Each program asserts its own expectations and prints OK. They need the mock
backend's in-memory server; with the real LibCarla backend they are skipped
(the CARLA integration suite covers that case).
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

UNIT = sorted((Path(__file__).resolve().parent / "unit").glob("test_*.codon"))
EXAMPLES = Path(__file__).resolve().parent.parent / "examples"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


@pytest.fixture(autouse=True)
def _require_mock(backend):
    if backend != "mock":
        pytest.skip(f"needs the mock backend (built: {backend})")


@pytest.mark.parametrize("source", UNIT, ids=lambda p: p.stem)
def test_unit(launcher, source):
    result = launcher("run", str(source))
    assert result.returncode == 0, ANSI.sub("", result.stdout + result.stderr)
    assert result.stdout.strip().endswith("OK"), result.stdout


@pytest.mark.parametrize("example", sorted(EXAMPLES.glob("*.py")), ids=lambda p: p.stem)
def test_example_runs(launcher, example):
    result = launcher("run", str(example))
    assert result.returncode == 0, ANSI.sub("", result.stdout + result.stderr)


def test_built_executable_is_self_contained(launcher, tmp_path):
    """A `build` output runs without the launcher and without libpython."""
    import os
    import subprocess

    exe = tmp_path / "connect"
    result = launcher("build", "-release", "-o", str(exe), str(EXAMPLES / "connect.py"))
    assert result.returncode == 0, ANSI.sub("", result.stderr)
    env = {k: v for k, v in os.environ.items()
           if k not in ("TYPESAFE_CARLA_LIB", "LD_LIBRARY_PATH", "CODON_PATH")}
    run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
    assert run.returncode == 0, run.stderr
    assert "vehicle.tesla.model3" in run.stdout
    ldd = subprocess.run(["ldd", str(exe)], capture_output=True, text=True).stdout
    assert "libpython" not in ldd, ldd
