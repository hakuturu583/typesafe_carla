"""`import typesafe_carla.carla as carla`: where the CPython package is built and
loaded from (typesafe_carla.carla_build), and the package itself.

The package tests need a build of `typesafe_carla.carla` (`typesafe-codon
pycarla`, 15-50 min): they run when TSC_PYCARLA_LIBRARY_DIR names one (it is
used as TYPESAFE_CARLA_PYCARLA_DIR) and are skipped otherwise.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

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
    assert out.name == f"{__version__}-{carla_build.cache_key()[:16]}"


def _bundled_codon(monkeypatch, tmp_path):
    from typesafe_carla import toolchain

    tc = toolchain.Toolchain(tmp_path / "bin" / "codon", tmp_path, "typesafe-carla-toolchain")
    monkeypatch.setattr(toolchain, "find_codon", lambda: tc)
    return tc


def test_the_cache_is_shared_by_installations(monkeypatch, tmp_path):
    # One build serves every installation (virtual environment) with the same
    # sources and toolchain: the key names no native library, by path or by
    # content (the build loads the importing installation's at run time).
    from typesafe_carla import paths

    _bundled_codon(monkeypatch, tmp_path)
    monkeypatch.delenv(carla_build.ENV_DIR, raising=False)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    dirs = set()
    for venv, content in (("a", b"one build"), ("b", b"another build")):
        lib = tmp_path / venv / paths.LIBRARY_NAME
        lib.parent.mkdir()
        lib.write_bytes(content)
        monkeypatch.setenv(paths.ENV_LIB, str(lib))
        dirs.add(carla_build.build_dir())
        assert carla_build.cache_key() == carla_build.prebuilt_stamp()
    assert len(dirs) == 1


def test_another_codon_has_its_own_cache_key(monkeypatch, tmp_path):
    from typesafe_carla import toolchain

    runtime = tmp_path / "lib" / "codon" / "libcodonrt.so"
    runtime.parent.mkdir(parents=True)
    runtime.write_bytes(b"runtime 1")
    tc = toolchain.Toolchain(tmp_path / "bin" / "codon", tmp_path, "PATH")
    monkeypatch.setattr(toolchain, "find_codon", lambda: tc)
    monkeypatch.setattr(toolchain.Toolchain, "version", lambda self: "0.19.3")
    first = carla_build.cache_key()
    assert first != carla_build.prebuilt_stamp()
    runtime.write_bytes(b"runtime 2")
    assert carla_build.cache_key() != first
    monkeypatch.setattr(toolchain.Toolchain, "version", lambda self: "0.19.4")
    runtime.write_bytes(b"runtime 1")
    assert carla_build.cache_key() != first


def _fake_build(seen):
    def build(out, **kwargs):
        seen.update(kwargs)
        pkg = out / "carla"
        pkg.mkdir(parents=True, exist_ok=True)
        (pkg / f"{pycarla.MODULE}.so").write_bytes(b"")
        (pkg / "_runtime.py").write_bytes(b"")
        (pkg / "_spec.json").write_text(json.dumps({"native_library": "/builder/lib.so",
                                                    "package": pycarla.LIBRARY_PACKAGE}))
        (out / f"{pycarla.MODULE}.o").write_bytes(b"")
        return out
    return build


def test_a_cache_build_names_no_path_of_the_installation_that_made_it(monkeypatch, tmp_path):
    # No RPATH to the builder's Codon runtime and no native library in
    # _spec.json: prepare_runtime() gives the build the importing
    # installation's, so a shared build cannot load another one's.
    seen = {}
    monkeypatch.setattr(pycarla, "build", _fake_build(seen))
    out = tmp_path / "out"
    carla_build._build(out, log=lambda *a: None)
    assert seen["rpath"] == []
    assert "native_library" not in json.loads((out / "carla" / "_spec.json").read_text())
    assert carla_build.is_current(out)
    assert sorted(p.name for p in out.iterdir()) == sorted(["carla", pycarla.STAMP, "pruned.json"])


def test_the_cli_builds_the_library_package_like_the_cache(monkeypatch, tmp_path):
    # `typesafe-codon pycarla -o DIR` makes what TYPESAFE_CARLA_PYCARLA_DIR=DIR loads.
    seen = {}
    monkeypatch.setattr(pycarla, "build", _fake_build(seen))
    assert pycarla.main(["-o", str(tmp_path / "out")]) == 0
    assert seen["rpath"] == []
    assert carla_build.is_current(tmp_path / "out")


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


def test_the_prebuilt_package_is_built_for_any_cpu(monkeypatch, tmp_path):
    # Codon targets the build machine's CPU by default: a prebuilt package
    # compiled on an AVX-512 machine dies with SIGILL on one without it.
    seen = {}
    monkeypatch.setattr(pycarla, "build", _fake_build(seen))
    monkeypatch.setattr(carla_build, "_toolchain_version", lambda: "0.19.3")
    prebuilt = carla_build.make_prebuilt(tmp_path / "_prebuilt", log=lambda *a: None)
    assert seen["portable"] is True
    assert seen["rpath"] == [carla_build.PREBUILT_RPATH]
    # The toolchain release beside the stamp, for explain_prebuilt_mismatch().
    assert (prebuilt / carla_build.TOOLCHAIN_FILE).read_text() == "0.19.3\n"
    assert "native_library" not in json.loads((prebuilt / "_spec.json").read_text())


@pytest.mark.parametrize("portable", [False, True])
def test_a_portable_compile_disables_native_code(monkeypatch, tmp_path, portable):
    calls = []
    monkeypatch.setattr(pycarla, "typesafe_codon", lambda *args, **kwargs: calls.append(args))
    pycarla._compile(tmp_path / "m.codon", tmp_path / "m.o", None, portable)
    assert ("--disable-native" in calls[0]) is portable


def _record_hash(data: bytes) -> str:
    return base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def _installed_toolchain(monkeypatch, version):
    """typesafe-carla-toolchain `version` is installed (to the stamp, too)."""
    from importlib import metadata

    real = metadata.version
    monkeypatch.setattr(metadata, "version", lambda name: version
                        if name == carla_build.TOOLCHAIN_DISTRIBUTION else real(name))


@pytest.fixture
def installed_wheel(monkeypatch, tmp_path):
    """A typesafe-carla 9.9.9 wheel installed into `tmp_path/site` (RECORD and
    all), with a prebuilt package, as what carla_build and pycarla look at."""
    site = tmp_path / "site"
    files = {
        "typesafe_carla/_codon/typesafe_carla/__init__.codon": b"__version__ = '9.9.9'\n",
        "typesafe_carla/_codon/typesafe_carla/client.codon": b"class Client: pass\n",
        "typesafe_carla/pycarla/__init__.py": b"# generator\n",
        "typesafe_carla/pycarla/_runtime.py": b"# runtime\n",
        "typesafe_carla/pycarla/pruned.json": b"{}\n",
        "typesafe_carla/carla/_prebuilt/_carla.so": b"\x7fELF",
        "typesafe_carla/carla/_prebuilt/stamp": b"set below\n",
    }
    for name, data in files.items():
        (site / name).parent.mkdir(parents=True, exist_ok=True)
        (site / name).write_bytes(data)
    info = site / "typesafe_carla-9.9.9.dist-info"
    info.mkdir()
    (info / "METADATA").write_text("Metadata-Version: 2.1\nName: typesafe-carla\nVersion: 9.9.9\n")
    record = [f"{n},sha256={_record_hash(d)},{len(d)}" for n, d in files.items()]
    (info / "RECORD").write_text("\n".join(record + [f"{info.name}/RECORD,,"]) + "\n")
    monkeypatch.syspath_prepend(str(site))
    pkg = site / "typesafe_carla"
    monkeypatch.setattr(pycarla, "PACKAGE", pkg / "_codon" / "typesafe_carla")
    monkeypatch.setattr(pycarla, "__file__", str(pkg / "pycarla" / "__init__.py"))
    monkeypatch.setattr(pycarla, "RUNTIME", pkg / "pycarla" / "_runtime.py")
    monkeypatch.setattr(pycarla, "PRUNED", pkg / "pycarla" / "pruned.json")
    monkeypatch.setattr(carla_build, "PREBUILT_DIR", pkg / "carla" / "_prebuilt")
    _installed_toolchain(monkeypatch, "0.19.3")
    monkeypatch.delenv(carla_build.ENV_DIR, raising=False)
    monkeypatch.delenv("PYCARLA_PRUNED", raising=False)
    # Built from exactly these files, as the release workflow builds it.
    (pkg / "carla" / "_prebuilt" / "stamp").write_text(carla_build.prebuilt_stamp() + "\n")
    return pkg


def test_a_matching_prebuilt_package_needs_no_explanation(installed_wheel):
    assert carla_build.prebuilt_is_current()
    assert carla_build.explain_prebuilt_mismatch() == []


def test_a_modified_installation_is_named(installed_wheel):
    # The case behind this check: a uv cache whose copy of the wheel was edited
    # through a hardlink. Every environment then silently built for 15-50 min.
    (installed_wheel / "pycarla" / "_runtime.py").write_text("# patched in place\n")
    (installed_wheel / "_codon" / "typesafe_carla" / "client.codon").unlink()
    reasons = carla_build.explain_prebuilt_mismatch()
    assert len(reasons) == 1, reasons
    assert "typesafe_carla/pycarla/_runtime.py" in reasons[0]
    assert "typesafe_carla/_codon/typesafe_carla/client.codon (missing)" in reasons[0]
    assert "typesafe-carla 9.9.9 wheel" in reasons[0]
    assert "uv cache clean typesafe-carla" in reasons[0]
    assert "pycarla/__init__.py" not in reasons[0]


def test_a_file_the_wheel_did_not_install_is_named(installed_wheel):
    (installed_wheel / "_codon" / "typesafe_carla" / "extra.codon").write_text("x = 1\n")
    reasons = carla_build.explain_prebuilt_mismatch()
    assert any("extra.codon is not a file the typesafe-carla 9.9.9 wheel installed" in r
               for r in reasons), reasons


def test_another_toolchain_release_is_named(installed_wheel, monkeypatch):
    (installed_wheel / "carla" / "_prebuilt" / carla_build.TOOLCHAIN_FILE).write_text("0.19.3\n")
    _installed_toolchain(monkeypatch, "0.19.4")
    assert not carla_build.prebuilt_is_current()
    assert carla_build.explain_prebuilt_mismatch() == [
        "the prebuilt package was built with typesafe-carla-toolchain 0.19.3, and 0.19.4 is "
        "installed: install typesafe-carla-toolchain==0.19.3 to use it"]


def test_an_unrecorded_toolchain_release_is_the_likely_cause(installed_wheel, monkeypatch):
    # Wheels up to 0.3.0 do not record their toolchain release.
    _installed_toolchain(monkeypatch, "0.19.4")
    (reason,) = carla_build.explain_prebuilt_mismatch()
    assert "the installed files are the typesafe-carla 9.9.9 wheel's" in reason
    assert "0.19.4" in reason


def test_the_build_says_why_the_prebuilt_package_is_not_used(installed_wheel, monkeypatch, tmp_path):
    (installed_wheel / "pycarla" / "_runtime.py").write_text("# patched in place\n")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    with pytest.raises(ImportError, match=r"(?s)typesafe-codon pycarla.*_runtime\.py differs"):
        carla_build.package_dir()
    logged = []
    monkeypatch.setenv(carla_build.ENV_BUILD, "1")
    monkeypatch.setattr(carla_build, "_build", lambda out, log: None)
    carla_build.ensure_built(log=logged.append)
    assert len(logged) == 1 and "_runtime.py differs" in logged[0]


def test_an_explicit_build_dir_is_not_a_mismatch(installed_wheel, monkeypatch, tmp_path):
    (installed_wheel / "pycarla" / "_runtime.py").write_text("# patched in place\n")
    monkeypatch.setenv(carla_build.ENV_DIR, str(tmp_path / "mine"))
    monkeypatch.setenv(carla_build.ENV_BUILD, "0")
    with pytest.raises(ImportError) as e:
        carla_build.package_dir()
    assert "prebuilt" not in str(e.value)


def test_a_build_without_rpath_loads_through_prepare_runtime(launcher, tmp_path):
    # What a cache build relies on: `_carla.so` linked with no RPATH finds the
    # Codon runtime only because prepare_runtime() has loaded this
    # installation's, so no other installation's can be picked up.
    import shutil

    if shutil.which("cc") is None:
        pytest.skip("needs cc to link the extension")
    source = tmp_path / "tiny.codon"
    source.write_text("def twice(x: int) -> int:\n    return 2 * x\n")
    obj = tmp_path / "tiny.o"
    result = launcher("build", "--pyext", "--relocation-model=pic", "--module", "tiny",
                      "-o", str(obj), str(source))
    assert result.returncode == 0, result.stdout + result.stderr
    pycarla.link(obj, tmp_path / "tiny.so", rpath=[])
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}

    def run(prelude: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-c", prelude + "import tiny; print(tiny.twice(21))"],
                              cwd=tmp_path, env=env, capture_output=True, text=True, timeout=60)

    alone = run("")
    assert alone.returncode != 0 and "libcodonrt" in alone.stderr, alone.stdout + alone.stderr
    prepared = run("from typesafe_carla import carla_build; carla_build.prepare_runtime()\n")
    assert prepared.returncode == 0 and prepared.stdout.split() == ["42"], \
        prepared.stdout + prepared.stderr
