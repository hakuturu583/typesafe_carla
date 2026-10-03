"""TSC_CARLA_PREBUILT=auto (cmake/LibCarlaPrebuilt.cmake,
tsc_fetch_libcarla_prebuilt) in a scratch CMake project, with the download
helper replaced by a fake: the answer is cached per ref and ABI fingerprint,
and a cached prefix that no longer fits is looked up again and given up on,
never failing the configure step."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SHA = "ada75f920642e18cace0a9f85ecf9d4077ddb531"

FAKE_HELPER = """\
import pathlib, sys
here = pathlib.Path(__file__).parent
with open(here / "calls.log", "a") as f:
    f.write(" ".join(sys.argv[1:]) + "\\n")
mode = (here / "mode").read_text().strip()
if mode == "none":
    print("CI has no LibCarla prebuilt (fake)", file=sys.stderr)
    sys.exit(2)
print(here / "prefix")
"""

PROJECT = """\
cmake_minimum_required(VERSION 3.27.2)
project(prebuilt_auto LANGUAGES C CXX)
set(TSC_CARLA_GIT_REPOSITORY "https://github.com/carla-simulator/carla.git")
set(TSC_CARLA_GIT_REF "{sha}")
option(TSC_CARLA_REFRESH "" OFF)
include("{root}/cmake/LibCarlaPrebuilt.cmake")
set(_TSC_PB_FETCH "{helper}")
tsc_abi_fingerprint(_abi)
file(WRITE "${{CMAKE_BINARY_DIR}}/abi.txt" "${{_abi}}")
tsc_fetch_libcarla_prebuilt(_prefix _sha)
message(STATUS "RESULT=[${{_prefix}}] SHA=[${{_sha}}]")
if(TSC_CARLA_REFRESH)
  set_property(CACHE TSC_CARLA_REFRESH PROPERTY VALUE OFF)
endif()
"""


def _cmake_ok() -> bool:
    cmake = shutil.which("cmake")
    if not cmake:
        return False
    out = subprocess.run([cmake, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (3, 27)


pytestmark = pytest.mark.skipif(not _cmake_ok(), reason="needs CMake >= 3.27")


@pytest.fixture
def project(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "fake_fetch.py").write_text(FAKE_HELPER)
    src = tmp_path / "src"
    src.mkdir()
    (src / "CMakeLists.txt").write_text(PROJECT.format(
        sha=SHA, root=ROOT.as_posix(), helper=(work / "fake_fetch.py").as_posix()))
    build = tmp_path / "build"

    def configure(mode: str, *args: str) -> tuple[str, str, int]:
        (work / "mode").write_text(mode)
        res = subprocess.run(["cmake", "-S", str(src), "-B", str(build),
                              f"-DPython3_EXECUTABLE={sys.executable}", *args],
                             capture_output=True, text=True)
        assert res.returncode == 0, res.stdout + res.stderr
        m = re.search(r"RESULT=\[(.*)\] SHA=\[(.*)\]", res.stdout)
        calls = (work / "calls.log").read_text().count("\n") if (work / "calls.log").exists() else 0
        return m.group(1), res.stdout + res.stderr, calls

    def make_prefix(abi: str) -> Path:
        prefix = work / "prefix"
        for rel in ("cmake/libcarla-targets.cmake", "licenses/LICENSE.CARLA",
                    "licenses/THIRD_PARTY_NOTICES.components"):
            (prefix / rel).parent.mkdir(parents=True, exist_ok=True)
            (prefix / rel).write_text("x\n")
        (prefix / "prebuilt.json").write_text(json.dumps({
            "format": 1, "abi": abi, "carla_repository": "https://github.com/carla-simulator/carla.git",
            "carla_ref": "0.10.0", "carla_commit": SHA, "carla_version": "0.10.0", "files": {}}))
        return prefix

    return configure, make_prefix, build


def test_auto_caches_and_falls_back(project):
    configure, make_prefix, build = project

    # Nothing published: source build, remembered.
    prefix, _, calls = configure("none")
    assert prefix == "" and calls == 1
    prefix, out, calls = configure("found")
    assert prefix == "" and calls == 1 and "as before" in out

    # TSC_CARLA_REFRESH looks again and finds one; later configures reuse it.
    good = make_prefix((build / "abi.txt").read_text())
    prefix, _, calls = configure("found", "-DTSC_CARLA_REFRESH=ON")
    assert prefix == str(good) and calls == 2
    prefix, _, calls = configure("found")
    assert prefix == str(good) and calls == 2

    # The cached prefix no longer fits (as after a compiler upgrade): looked
    # up again, and with nothing published, a source build, not an error.
    make_prefix("target=x86_64-linux-gnu\ncc=gcc 11.3.0")
    prefix, out, calls = configure("none")
    assert prefix == "" and calls == 3 and "no longer fits" in out

    # A download that does not fit is not used either.
    prefix, out, calls = configure("found", "-DTSC_CARLA_REFRESH=ON")
    assert prefix == "" and calls == 4 and "does not fit" in out
