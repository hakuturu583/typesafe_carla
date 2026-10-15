"""LibCarla patches (cmake/carla-patches): the patch id, the same in Python
(tools/carla_patches.py) and CMake (cmake/CarlaPatches.cmake), and how
cmake/FetchCarla.cmake applies them, against a local stand-in for the CARLA
repository (no network, no LibCarla build).

That the patches apply to real CARLA refs is checked by CI's libcarla job
(it configures every ref it builds) and here, for local CARLA checkouts named
in TSC_CARLA_PATCH_CHECK_DIRS (os.pathsep-separated), e.g.

    TSC_CARLA_PATCH_CHECK_DIRS=~/carla-ue5-dev:~/carla-ue58-dev uv run pytest tests/test_carla_patches.py
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from tools import carla_patches

ROOT = Path(__file__).resolve().parent.parent
GIT = shutil.which("git")


def _cmake_ok() -> bool:
    cmake = shutil.which("cmake")
    if not cmake:
        return False
    out = subprocess.run([cmake, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (3, 27)


needs_cmake = pytest.mark.skipif(not _cmake_ok() or not GIT, reason="needs CMake >= 3.27 and git")


# --- The patch id ------------------------------------------------------------

def test_patch_id_none_without_patches(tmp_path):
    assert carla_patches.patch_id(tmp_path) == "none"
    assert carla_patches.patch_id(tmp_path / "missing") == "none"
    (tmp_path / "README.md").write_text("not a patch\n")
    (tmp_path / "dir.patch").mkdir()
    assert carla_patches.patch_id(tmp_path) == "none"


def test_patch_id_is_stable_and_follows_names_order_and_content(tmp_path):
    (tmp_path / "0001-a.patch").write_text("a\n")
    (tmp_path / "0002-b.patch").write_text("b\n")
    pid = carla_patches.patch_id(tmp_path)
    assert re.fullmatch(r"[0-9a-f]{12}", pid)
    assert carla_patches.patch_id(tmp_path) == pid
    # The definition, spelled out.
    import hashlib
    text = "".join(f"{n} {hashlib.sha256(c).hexdigest()}\n"
                   for n, c in (("0001-a.patch", b"a\n"), ("0002-b.patch", b"b\n")))
    assert pid == hashlib.sha256(text.encode()).hexdigest()[:12]
    # Other files do not count.
    (tmp_path / "README.md").write_text("notes\n")
    assert carla_patches.patch_id(tmp_path) == pid
    # The order (by name) counts: the same contents applied the other way round.
    (tmp_path / "0001-a.patch").write_text("b\n")
    (tmp_path / "0002-b.patch").write_text("a\n")
    assert carla_patches.patch_id(tmp_path) != pid
    # So do the contents.
    (tmp_path / "0001-a.patch").write_text("a\n")
    (tmp_path / "0002-b.patch").write_text("b \n")
    assert carla_patches.patch_id(tmp_path) != pid
    assert [p.name for p in carla_patches.patch_files(tmp_path)] == ["0001-a.patch", "0002-b.patch"]


def test_the_repository_has_patches():
    files = carla_patches.patch_files()
    assert files, "cmake/carla-patches has no patch"
    assert carla_patches.patch_id() != "none"
    for p in files:
        assert re.fullmatch(r"\d{4}-[a-z0-9-]+\.patch", p.name), p.name
        assert "diff --git a/LibCarla/" in p.read_text(), f"{p.name}: not a git diff of LibCarla/"
    readme = (carla_patches.PATCH_DIR / "README.md").read_text()
    for p in files:
        assert p.name in readme, f"{p.name} is not listed in cmake/carla-patches/README.md"


def _cmake_patch_id(directory: Path, work: Path) -> str:
    script = work / f"{directory.name}-id.cmake"
    script.write_text(f'set(_TSC_CARLA_PATCH_DIR "{directory.as_posix()}")\n'
                      f'include("{(ROOT / "cmake" / "CarlaPatches.cmake").as_posix()}")\n'
                      'tsc_carla_patch_id(_id)\nmessage(STATUS "ID=[${_id}]")\n')
    res = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    return re.search(r"ID=\[(.*)\]", res.stdout).group(1)


@needs_cmake
def test_cmake_computes_the_same_patch_id(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    two = tmp_path / "two"
    two.mkdir()
    (two / "0002-b.patch").write_bytes(b"b\r\n\xc3\xa9\n")
    (two / "0001-a.patch").write_text("a\n")
    (two / "README.md").write_text("x\n")
    for d in (empty, two, carla_patches.PATCH_DIR):
        assert _cmake_patch_id(d, tmp_path) == carla_patches.patch_id(d), d


# --- FetchCarla: applying them ----------------------------------------------

PROJECT = """\
cmake_minimum_required(VERSION 3.27.2)
project(fetch_carla LANGUAGES NONE)
set(_TSC_CARLA_PATCH_DIR "{patches}")
include("{root}/cmake/FetchCarla.cmake")
file(READ "${{TSC_CARLA_DIR}}/LibCarla/source/Leak.cpp" _src)
string(STRIP "${{_src}}" _src)
message(STATUS "PATCHES=[${{TSC_CARLA_PATCHES}}] SRC=[${{_src}}]")
"""

PATCH_A = """\
Description before the diff, which git apply ignores.

diff --git a/LibCarla/source/Leak.cpp b/LibCarla/source/Leak.cpp
--- a/LibCarla/source/Leak.cpp
+++ b/LibCarla/source/Leak.cpp
@@ -1 +1 @@
-leak
+fixed
"""

PATCH_B = """\
diff --git a/LibCarla/source/Leak.cpp b/LibCarla/source/Leak.cpp
--- a/LibCarla/source/Leak.cpp
+++ b/LibCarla/source/Leak.cpp
@@ -1 +1 @@
-fixed
+fixed twice
"""


def _git(cwd: Path, *args: str) -> str:
    return subprocess.run([GIT, *args], cwd=cwd, check=True, capture_output=True,
                          text=True).stdout.strip()


@pytest.fixture
def fetch(tmp_path, monkeypatch):
    # A stand-in for the CARLA repository: what _tsc_check_ue5 looks for, and
    # one source file for the patches to change.
    upstream = tmp_path / "carla"
    for rel, text in (("CMakeLists.txt", ""), ("CMake/Options.cmake", ""),
                      ("LibCarla/CMakeLists.txt", ""), ("LibCarla/source/Leak.cpp", "leak\n"),
                      ("LICENSE", "MIT\n")):
        (upstream / rel).parent.mkdir(parents=True, exist_ok=True)
        (upstream / rel).write_text(text)
    _git(upstream, "init", "-q", "-b", "ue5-dev")
    _git(upstream, "add", ".")
    _git(upstream, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "base")
    sha = _git(upstream, "rev-parse", "HEAD")
    # file:// so that the shallow, filtered fetch is done as for GitHub.
    _git(upstream, "config", "uploadpack.allowFilter", "true")

    patches = tmp_path / "patches"
    patches.mkdir()
    src = tmp_path / "src"
    src.mkdir()
    (src / "CMakeLists.txt").write_text(PROJECT.format(patches=patches.as_posix(),
                                                       root=ROOT.as_posix()))
    build = tmp_path / "build"
    for var in ("CARLA_SOURCE_DIR", "CARLA_GIT_REF", "CARLA_REF_NAME", "TSC_CARLA_PREBUILT",
                "TSC_CARLA_PREBUILT_DIR"):
        monkeypatch.delenv(var, raising=False)

    def configure(*args: str, ok: bool = True) -> tuple[str, str, str]:
        res = subprocess.run(["cmake", "-S", str(src), "-B", str(build),
                              f"-DTSC_CARLA_GIT_REPOSITORY=file://{upstream.as_posix()}",
                              "-DTSC_CARLA_GIT_REF=ue5-dev", *args],
                             capture_output=True, text=True)
        out = res.stdout + res.stderr
        assert (res.returncode == 0) == ok, out
        m = re.search(r"PATCHES=\[(.*)\] SRC=\[(.*)\]", res.stdout)
        return (m.group(1), m.group(2), out) if m else ("", "", out)

    return configure, patches, upstream, build, sha


@needs_cmake
def test_fetch_applies_patches_once(fetch):
    configure, patches, _, build, _ = fetch
    (patches / "0001-a.patch").write_text(PATCH_A)
    pid, src, out = configure()
    assert pid == carla_patches.patch_id(patches) != "none"
    assert src == "fixed"
    assert "fetching CARLA" in out and "applied the LibCarla patch 0001-a.patch" in out
    assert f"patches={pid}" in (build / "_deps" / "carla-src.ref").read_text()

    # Unchanged: neither fetched nor patched again (git apply would fail on
    # the patched file).
    pid2, src, out = configure()
    assert (pid2, src) == (pid, "fixed")
    assert "fetching CARLA" not in out and "applied" not in out

    # Another set of patches: fetched afresh and patched with both.
    (patches / "0002-b.patch").write_text(PATCH_B)
    pid3, src, out = configure()
    assert pid3 == carla_patches.patch_id(patches) not in (pid, "none")
    assert src == "fixed twice"
    assert "fetching CARLA" in out


@needs_cmake
def test_a_checkout_from_before_the_patches_is_refetched(fetch):
    configure, patches, _, build, _ = fetch
    pid, src, out = configure()
    assert (pid, src) == ("none", "leak")
    # A stamp as FetchCarla wrote it before there were patches.
    stamp = build / "_deps" / "carla-src.ref"
    stamp.write_text(stamp.read_text().split(" patches=")[0])
    (patches / "0001-a.patch").write_text(PATCH_A)
    pid, src, out = configure()
    assert pid != "none" and src == "fixed" and "fetching CARLA" in out


@needs_cmake
def test_a_patch_that_does_not_apply_fails_and_is_retried(fetch):
    configure, patches, _, build, _ = fetch
    (patches / "0001-a.patch").write_text(PATCH_B)  # expects "fixed"
    _, _, out = configure(ok=False)
    assert "cmake/carla-patches/0001-a.patch does not apply" in " ".join(out.split())
    assert not (build / "_deps" / "carla-src.ref").exists()
    (patches / "0001-a.patch").write_text(PATCH_A)
    pid, src, out = configure()
    assert src == "fixed" and "fetching CARLA" in out


@needs_cmake
def test_a_patch_the_ref_already_has_is_skipped(fetch):
    configure, patches, upstream, _, _ = fetch
    # Upstream took the fix.
    (upstream / "LibCarla/source/Leak.cpp").write_text("fixed\n")
    _git(upstream, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-am", "fix")
    (patches / "0001-a.patch").write_text(PATCH_A)
    pid, src, out = configure()
    assert pid != "none" and src == "fixed"
    assert "already has the LibCarla patch 0001-a.patch: skipped" in " ".join(out.split())


@needs_cmake
def test_a_local_source_tree_is_not_patched(fetch, tmp_path):
    configure, patches, upstream, _, _ = fetch
    (patches / "0001-a.patch").write_text(PATCH_A)
    pid, src, out = configure(f"-DTSC_CARLA_SOURCE_DIR={upstream}")
    assert (pid, src) == ("none", "leak")
    assert _git(upstream, "status", "--porcelain") == ""


# --- Real CARLA checkouts ------------------------------------------------------

_CHECK_DIRS = [Path(d).expanduser() for d in
               os.environ.get("TSC_CARLA_PATCH_CHECK_DIRS", "").split(os.pathsep) if d]


@pytest.mark.skipif(not _CHECK_DIRS or not GIT,
                    reason="set TSC_CARLA_PATCH_CHECK_DIRS to CARLA checkouts to check")
@pytest.mark.parametrize("carla", _CHECK_DIRS, ids=str)
def test_patches_apply_to_carla(carla):
    """Every patch applies, in order, to the CARLA checkout (left unchanged),
    or the checkout has it already."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        # A copy of the files the patches touch, so that they apply one after
        # another without changing the checkout.
        copy = Path(tmp)
        for p in carla_patches.patch_files():
            for rel in re.findall(r"^diff --git a/(\S+) ", p.read_text(), re.M):
                if (carla / rel).is_file() and not (copy / rel).exists():
                    (copy / rel).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(carla / rel, copy / rel)
        for p in carla_patches.patch_files():
            res = subprocess.run([GIT, "apply", "--verbose", str(p)], cwd=copy,
                                 capture_output=True, text=True)
            if res.returncode != 0 and subprocess.run(
                    [GIT, "apply", "--reverse", "--check", str(p)], cwd=copy,
                    capture_output=True).returncode == 0:
                continue  # upstream has it
            assert res.returncode == 0, f"{p.name} does not apply to {carla}:\n{res.stderr}"
