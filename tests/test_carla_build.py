"""`import typesafe_carla.carla as carla`: where the CPython package is built and
loaded from (typesafe_carla.carla_build), and the package itself.

The package tests need a build of `typesafe_carla.carla` (`typesafe-codon
pycarla`, ~15 min): they run when TSC_PYCARLA_LIBRARY_DIR names one (it is
used as TYPESAFE_CARLA_PYCARLA_DIR) and are skipped otherwise.
"""

from __future__ import annotations

import os
import subprocess
import sys

import pytest

from typesafe_carla import carla_build, pycarla


def test_explicit_build_dir(monkeypatch, tmp_path):
    monkeypatch.setenv(carla_build.ENV_DIR, str(tmp_path / "here"))
    assert carla_build.build_dir() == tmp_path / "here"


def test_default_build_dir_is_keyed_by_the_sources(monkeypatch, tmp_path):
    from typesafe_carla import __version__

    monkeypatch.delenv(carla_build.ENV_DIR, raising=False)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path))
    out = carla_build.build_dir()
    assert out.parent == tmp_path / "typesafe_carla" / "pycarla"
    assert out.name == f"{__version__}-{pycarla.source_stamp(pycarla.LIBRARY_PACKAGE)[:16]}"


def test_the_key_depends_on_the_package_name():
    assert pycarla.source_stamp("carla") != pycarla.source_stamp(pycarla.LIBRARY_PACKAGE)


def test_no_build_on_import_when_disabled(monkeypatch, tmp_path):
    monkeypatch.setenv(carla_build.ENV_DIR, str(tmp_path / "empty"))
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    with pytest.raises(ImportError, match="typesafe-codon pycarla"):
        carla_build.ensure_built()
    assert not (tmp_path / "empty").exists()


def test_a_stale_build_is_not_current(tmp_path):
    (tmp_path / "carla").mkdir()
    (tmp_path / "carla" / f"{pycarla.MODULE}.so").write_bytes(b"")
    (tmp_path / pycarla.STAMP).write_text("an older build\n")
    assert not carla_build.is_current(tmp_path)


def test_the_cli_runs_the_generator(monkeypatch):
    from typesafe_carla import cli

    seen = []
    monkeypatch.setattr(pycarla, "main", lambda argv: seen.append(argv) or 0)
    assert cli.main(["pycarla", "--package", "carla"]) == 0
    assert seen == [["--package", "carla"]]


def _fake_prebuilt(directory, stamp):
    directory.mkdir(parents=True)
    (directory / f"{pycarla.MODULE}.so").write_bytes(b"")
    (directory / pycarla.STAMP).write_text(stamp + "\n")
    return directory


def test_the_prebuilt_package_is_loaded_when_it_matches(monkeypatch, tmp_path):
    prebuilt = _fake_prebuilt(tmp_path / "_prebuilt", carla_build.prebuilt_stamp())
    monkeypatch.setattr(carla_build, "PREBUILT_DIR", prebuilt)
    monkeypatch.delenv(carla_build.ENV_DIR, raising=False)
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    assert carla_build.package_dir() == prebuilt


def test_a_stale_prebuilt_package_is_not_loaded(monkeypatch, tmp_path):
    # Edited sources or another toolchain release: build instead (here: refuse).
    prebuilt = _fake_prebuilt(tmp_path / "_prebuilt", "another release")
    monkeypatch.setattr(carla_build, "PREBUILT_DIR", prebuilt)
    monkeypatch.setenv(carla_build.ENV_DIR, str(tmp_path / "cache"))
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    assert not carla_build.prebuilt_is_current(prebuilt)
    monkeypatch.delenv(carla_build.ENV_DIR)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    with pytest.raises(ImportError):
        carla_build.package_dir()


def test_the_build_dir_wins_over_the_prebuilt_package(monkeypatch, tmp_path):
    prebuilt = _fake_prebuilt(tmp_path / "_prebuilt", carla_build.prebuilt_stamp())
    monkeypatch.setattr(carla_build, "PREBUILT_DIR", prebuilt)
    monkeypatch.setenv(carla_build.ENV_DIR, str(tmp_path / "mine"))
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    with pytest.raises(ImportError, match="mine"):
        carla_build.package_dir()


def test_the_prebuilt_key_ignores_the_native_library():
    assert carla_build.prebuilt_stamp() == pycarla.source_stamp(pycarla.LIBRARY_PACKAGE, native=False)
    assert carla_build.prebuilt_stamp() != pycarla.source_stamp(pycarla.LIBRARY_PACKAGE)


def test_adding_the_prebuilt_package_to_a_wheel(tmp_path):
    import base64
    import hashlib
    import zipfile

    from tools import add_pycarla_to_wheel as tool

    wheel = tmp_path / "typesafe_carla-9.9.9-py3-none-any.whl"
    with zipfile.ZipFile(wheel, "w") as z:
        z.writestr("typesafe_carla/__init__.py", "x = 1\n")
        z.writestr("typesafe_carla-9.9.9.dist-info/RECORD",
                   "typesafe_carla/__init__.py,sha256=abc,6\ntypesafe_carla-9.9.9.dist-info/RECORD,,\n")
    prebuilt = tmp_path / "_prebuilt"
    prebuilt.mkdir()
    (prebuilt / "_carla.so").write_bytes(b"\x7fELF")
    (prebuilt / "stamp").write_text("k\n")
    tool.add(wheel, prebuilt)
    tool.add(wheel, prebuilt)  # idempotent: replaces, does not duplicate
    with zipfile.ZipFile(wheel) as z:
        names = z.namelist()
        record = z.read("typesafe_carla-9.9.9.dist-info/RECORD").decode().splitlines()
        so = z.read(f"{tool.PREBUILT}/_carla.so")
    assert names.count(f"{tool.PREBUILT}/_carla.so") == 1
    digest = base64.urlsafe_b64encode(hashlib.sha256(so).digest()).rstrip(b"=").decode()
    assert f"{tool.PREBUILT}/_carla.so,sha256={digest},{len(so)}" in record
    assert record[-1] == "typesafe_carla-9.9.9.dist-info/RECORD,,"
    assert sum(ln.startswith(tool.PREBUILT) for ln in record) == 2


@pytest.fixture(scope="module")
def library_build():
    out = os.environ.get("TSC_PYCARLA_LIBRARY_DIR")
    if not out:
        pytest.skip("needs a build of typesafe_carla.carla: TSC_PYCARLA_LIBRARY_DIR "
                    "(`typesafe-codon pycarla -o DIR`)")
    return out


def _run(out: str, program: str) -> str:
    env = {**os.environ, carla_build.ENV_DIR: out, carla_build.ENV_BUILD: "0"}
    result = subprocess.run([sys.executable, "-c", program], env=env, capture_output=True,
                            text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_the_package_has_the_python_api_names(library_build):
    out = _run(library_build, (
        "import typesafe_carla.carla as carla\n"
        "loc = carla.Location(1.0, 2.0, 3.0)\n"
        "t = carla.Transform(loc, carla.Rotation(yaw=90.0))\n"
        "assert isinstance(loc, carla.Vector3D)\n"
        "assert carla.Location.__module__ == 'typesafe_carla.carla'\n"
        "assert carla.command.DestroyActor is not None\n"
        "assert int(carla.TrafficLightState.Red) == 0\n"
        "print(t.location.y, loc.distance(carla.Location()) > 3.7)\n"))
    assert out.split() == ["2.0", "True"]


def test_the_package_does_not_take_the_carla_name(library_build):
    out = _run(library_build, (
        "import sys\n"
        "import typesafe_carla.carla as carla\n"
        "print('carla' in sys.modules, 'typesafe_carla.carla.command' in sys.modules)\n"))
    assert out.split() == ["False", "True"]
