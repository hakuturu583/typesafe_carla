"""Runtime tests: Codon programs in tests/unit, run against the mock backend.

Each program asserts its own expectations and prints OK. They need the mock
backend's in-memory server; with the real LibCarla backend they are skipped
(the CARLA integration suite covers that case).
"""

from __future__ import annotations

from pathlib import Path

import pytest

UNIT = sorted((Path(__file__).resolve().parent / "unit").glob("test_*.codon"))
EXAMPLES = Path(__file__).resolve().parent.parent / "examples"


@pytest.fixture(autouse=True)
def _require_mock(backend):
    if backend != "mock":
        pytest.skip(f"needs the mock backend (built: {backend})")


@pytest.mark.parametrize("source", UNIT, ids=lambda p: p.stem)
def test_unit(launcher, source):
    result = launcher("run", str(source))
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip().endswith("OK"), result.stdout


@pytest.mark.parametrize("example", sorted(EXAMPLES.glob("*.py")), ids=lambda p: p.stem)
def test_example_runs(launcher, example):
    result = launcher("run", str(example))
    assert result.returncode == 0, result.stdout + result.stderr


def test_built_executable_is_self_contained(launcher, tmp_path):
    """A `build` output runs without the launcher and without libpython."""
    import os
    import subprocess

    exe = tmp_path / "connect"
    result = launcher("build", "-release", "-o", str(exe), str(EXAMPLES / "connect.py"))
    assert result.returncode == 0, result.stderr
    env = {k: v for k, v in os.environ.items()
           if k not in ("TYPESAFE_CARLA_LIB", "LD_LIBRARY_PATH", "CODON_PATH")}
    run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
    assert run.returncode == 0, run.stderr
    assert "vehicle.tesla.model3" in run.stdout
    ldd = subprocess.run(["ldd", str(exe)], capture_output=True, text=True).stdout
    assert "libpython" not in ldd, ldd


_COMPAT_PROGRAM = """
import typesafe_carla as carla
world = carla.Client("localhost", 2000).get_world()
actor = world.get_actor(world.get_actors()[0].id)
for i in range(3):
    actor.apply_control(carla.VehicleControl(throttle=0.1 * i))
print("OK")
"""

_STATIC_PROGRAM = _COMPAT_PROGRAM.replace("actor.apply_control", "actor.as_vehicle().apply_control")

_RUNTIME_WARNING = ("typesafe_carla: warning: Actor.apply_control(VehicleControl) without "
                    "as_vehicle() is a Python-API compatibility path")
_COMPILE_WARNING = ("typesafe-codon: compile-time warning: Actor.apply_control(VehicleControl) "
                    "without as_vehicle(): Python-API compatibility path")


def _program(tmp_path, text: str):
    source = tmp_path / "program.codon"
    source.write_text(text)
    return str(source)


def test_compat_path_warns_once(launcher, tmp_path):
    result = launcher("run", _program(tmp_path, _COMPAT_PROGRAM))
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "OK"
    assert result.stderr.count(_COMPILE_WARNING) == 1, result.stderr
    assert result.stderr.count(_RUNTIME_WARNING) == 1, result.stderr
    # The compile-time warning comes before the program runs.
    assert result.stderr.index(_COMPILE_WARNING) < result.stderr.index(_RUNTIME_WARNING)


def test_compat_warnings_can_be_silenced(launcher, tmp_path):
    result = launcher("run", _program(tmp_path, _COMPAT_PROGRAM),
                      env={"TYPESAFE_CARLA_COMPAT_WARNINGS": "0"})
    assert result.returncode == 0, result.stderr
    assert "warning" not in result.stderr, result.stderr


@pytest.mark.parametrize("strict", [False, True], ids=["default", "strict"])
def test_static_path_does_not_warn(launcher, tmp_path, strict):
    flags = ["--strict"] if strict else []
    result = launcher(*flags, "run", _program(tmp_path, _STATIC_PROGRAM))
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "OK"
    assert "warning" not in result.stderr, result.stderr


def test_program_without_compat_path_does_not_warn(launcher):
    result = launcher("run", str(EXAMPLES / "connect.py"))
    assert result.returncode == 0, result.stderr
    assert "warning" not in result.stderr, result.stderr


def test_strict_rejects_compat_path(launcher, tmp_path):
    source = _program(tmp_path, _COMPAT_PROGRAM)
    for result in (launcher("--strict", "run", source),
                   launcher("run", source, env={"TYPESAFE_CARLA_STRICT": "1"})):
        assert result.returncode != 0
        assert "strict mode: call as_vehicle() first" in result.stderr, result.stderr
        assert "compile-time warning" not in result.stderr


def test_build_warns_and_executable_warns_once(launcher, tmp_path):
    import subprocess

    exe = tmp_path / "program"
    result = launcher("build", "-o", str(exe), _program(tmp_path, _COMPAT_PROGRAM))
    assert result.returncode == 0, result.stderr
    assert result.stderr.count(_COMPILE_WARNING) == 1, result.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    assert run.stderr.count(_RUNTIME_WARNING) == 1, run.stderr
    ll = tmp_path / "program.ll"
    result = launcher("build", "--llvm", "-o", str(ll), _program(tmp_path, _COMPAT_PROGRAM))
    assert result.returncode == 0, result.stderr
    assert result.stderr.count(_COMPILE_WARNING) == 1, result.stderr


def test_program_arguments_reach_the_program(launcher, tmp_path):
    source = _program(tmp_path, _COMPAT_PROGRAM.replace(
        'print("OK")', 'import sys\nprint(sys.argv[1:])\nsys.exit(3)'))
    result = launcher("run", source, "a", "-b")
    assert result.returncode == 3, result.stderr
    assert result.stdout.strip() == "['a', '-b']"

COMPAT = Path(__file__).resolve().parent / "unit" / "compat"
_WARNING = "typesafe_carla: warning: "


def test_compat_warning_printed_once_per_api(launcher):
    """A Location passed as a Vector3D (or the reverse) warns once per API, on stderr (#10)."""
    result = launcher("run", str(COMPAT / "location_as_vector.codon"),
                      env={"TYPESAFE_CARLA_COMPAT_WARNINGS": "1", "TYPESAFE_CARLA_STRICT": "0"})
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip().endswith("OK")
    warnings = [l for l in result.stderr.splitlines() if l.startswith(_WARNING)]
    assert len(warnings) == 3, result.stderr
    assert "a Location passed as a Vector3D to Actor.set_target_velocity" in warnings[0]
    assert "a Location passed as a Vector3D to Actor.add_force" in warnings[1]
    assert "a Vector3D passed as a Location to Actor.set_location" in warnings[2]
    assert "carla.Location(v)" in warnings[2]
    assert "location.as_vector()" in warnings[0] and "[tsc-compat]" not in result.stderr
    compile_warnings = [l for l in result.stderr.splitlines()
                        if l.startswith("typesafe-codon: compile-time warning: ")]
    assert sorted(l.split(": ")[2] for l in compile_warnings) == [
        "a Location passed as a Vector3D to Actor.add_force",
        "a Location passed as a Vector3D to Actor.set_target_velocity",
        "a Vector3D passed as a Location to Actor.set_location"], result.stderr


def test_compat_warning_silenced(launcher):
    result = launcher("run", str(COMPAT / "location_as_vector.codon"),
                      env={"TYPESAFE_CARLA_COMPAT_WARNINGS": "0", "TYPESAFE_CARLA_STRICT": "0"})
    assert result.returncode == 0, result.stdout + result.stderr
    assert "warning" not in result.stderr, result.stderr


def test_no_compat_warning_for_explicit_conversion(launcher):
    result = launcher("run", str(COMPAT / "explicit_as_vector.codon"),
                      env={"TYPESAFE_CARLA_COMPAT_WARNINGS": "1", "TYPESAFE_CARLA_STRICT": "0"})
    assert result.returncode == 0, result.stdout + result.stderr
    assert "warning" not in result.stderr, result.stderr


def test_explicit_conversion_runs_in_strict_mode(launcher):
    result = launcher("--strict", "run", str(COMPAT / "explicit_as_vector.codon"))
    assert result.returncode == 0, result.stdout + result.stderr


def test_strict_rejects_location_as_vector(launcher):
    result = launcher("--strict", "run", str(COMPAT / "location_as_vector.codon"))
    assert result.returncode != 0
    assert ("strict mode: convert with location.as_vector() (Location as Vector3D is a "
            "Python-API compatibility shortcut)") in result.stderr, result.stderr


def test_compat_literal_only_in_programs_that_use_the_path(launcher, tmp_path):
    """The "[tsc-compat] " string constant is in the IR exactly when the path is used."""
    import re
    literal = re.compile(r'c"\[tsc-compat\] ([^"]*)\\00"')
    found = {}
    for name in ("location_as_vector", "explicit_as_vector"):
        ll = tmp_path / f"{name}.ll"
        result = launcher("build", "--llvm", "-o", str(ll), str(COMPAT / f"{name}.codon"),
                          env={"TYPESAFE_CARLA_STRICT": "0"})
        assert result.returncode == 0, result.stderr
        found[name] = set(literal.findall(ll.read_text()))
    assert found["location_as_vector"] == {
        "a Location passed as a Vector3D to Actor.set_target_velocity",
        "a Location passed as a Vector3D to Actor.add_force",
        "a Vector3D passed as a Location to Actor.set_location"}
    assert found["explicit_as_vector"] == set()
