"""Integration tests against a real CARLA (UE5) server.

Run with a server listening and the real backend built, e.g.:

    TSC_CARLA_PORT=2000 TYPESAFE_CARLA_BUILD_DIR=build-carla uv run pytest tests/test_integration.py

Skipped unless TSC_CARLA_PORT is set and the library is the libcarla backend.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

PROGRAMS = sorted((Path(__file__).resolve().parent / "integration").glob("test_*.codon"))
ANSI = re.compile(r"\x1b\[[0-9;]*m")


@pytest.fixture(autouse=True)
def _require_server(backend):
    if not os.environ.get("TSC_CARLA_PORT"):
        pytest.skip("set TSC_CARLA_PORT (and TSC_CARLA_HOST) to run against a CARLA server")
    if backend != "libcarla":
        pytest.skip(f"needs the libcarla backend (built: {backend})")


@pytest.mark.parametrize("source", PROGRAMS, ids=lambda p: p.stem)
def test_integration(launcher, source):
    result = launcher("run", "-release", str(source), timeout=900)
    output = ANSI.sub("", result.stdout + result.stderr)
    print(output)
    assert result.returncode == 0, output
    assert result.stdout.strip().endswith("OK"), output
