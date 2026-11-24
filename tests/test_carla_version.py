"""tsc_carla_version (cmake/CarlaVersion.cmake): the version a CARLA checkout
declares, with or without a PATCH component."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent

CARLA_0_10 = """\
cmake_minimum_required (VERSION 3.27.2)
set (CARLA_VERSION_MAJOR 0)
set (CARLA_VERSION_MINOR 10)
set (CARLA_VERSION_PATCH 0)
set (
  CARLA_VERSION
  ${CARLA_VERSION_MAJOR}.${CARLA_VERSION_MINOR}.${CARLA_VERSION_PATCH}
)
"""

# CARLA 1.0 (and ue58-dev) declare no PATCH.
CARLA_1_0 = """\
cmake_minimum_required (VERSION 3.28.0)
set (CARLA_VERSION_MAJOR 1)
set (CARLA_VERSION_MINOR 0)
set (
  CARLA_VERSION
  ${CARLA_VERSION_MAJOR}.${CARLA_VERSION_MINOR}
)
"""


def _version(tmp_path: Path, cmakelists: str) -> subprocess.CompletedProcess[str]:
    cmake = shutil.which("cmake")
    if cmake is None:
        pytest.skip("cmake is not installed")
    (tmp_path / "CMakeLists.txt").write_text(cmakelists)
    script = tmp_path / "read.cmake"
    script.write_text(
        f'include("{ROOT}/cmake/CarlaVersion.cmake")\n'
        f'tsc_carla_version("{tmp_path}/CMakeLists.txt" v)\n'
        'message(STATUS "version=[${v}]")\n'
    )
    return subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)


@pytest.mark.parametrize(
    ("cmakelists", "version"),
    [(CARLA_0_10, "0.10.0"), (CARLA_1_0, "1.0")],
)
def test_the_declared_version_is_read(
    tmp_path: Path, cmakelists: str, version: str
) -> None:
    res = _version(tmp_path, cmakelists)
    assert res.returncode == 0, res.stderr
    assert f"version=[{version}]" in res.stdout


def test_a_checkout_declaring_no_version_fails(tmp_path: Path) -> None:
    res = _version(tmp_path, "project(nothing)\n")
    assert res.returncode != 0
    assert "CARLA_VERSION_MAJOR" in res.stderr
