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
