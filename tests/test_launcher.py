from __future__ import annotations

from pathlib import Path

import pytest

from typesafe_carla import cli, paths, toolchain


def test_add_rpath_inserts_flag_after_build():
    args = cli.add_rpath(["build", "-release", "main.py"], "/opt/lib")
    assert args[0] == "build"
    assert args[1] == "--linker-flags=-Wl,--disable-new-dtags,-rpath,/opt/lib"
    assert args[2:] == ["-release", "main.py"]


@pytest.mark.parametrize("form", ["separate", "equals"])
def test_add_rpath_merges_existing_linker_flags(form):
    if form == "separate":
        args = cli.add_rpath(["build", "-linker-flags", "-lm", "main.py"], "/x")
        assert args == ["build", "-linker-flags", "-lm -Wl,--disable-new-dtags,-rpath,/x",
                        "main.py"]
    else:
        args = cli.add_rpath(["build", "--linker-flags=-lm", "main.py"], "/x")
        assert args == ["build", "--linker-flags=-lm -Wl,--disable-new-dtags,-rpath,/x",
                        "main.py"]


def test_codon_modules_dir_is_the_checkout():
    root = Path(__file__).resolve().parent.parent
    assert paths.codon_modules_dir() == root / "codon"


def test_native_library_override(tmp_path, monkeypatch):
    fake = tmp_path / "libfake.so"
    fake.write_bytes(b"")
    monkeypatch.setenv(paths.ENV_LIB, str(fake))
    assert paths.native_library() == fake.resolve()
    monkeypatch.setenv(paths.ENV_LIB, str(tmp_path / "missing.so"))
    with pytest.raises(paths.PathError):
        paths.native_library()


def test_explicit_codon_must_exist(tmp_path, monkeypatch):
    monkeypatch.setenv(toolchain.ENV_CODON, str(tmp_path / "nope"))
    with pytest.raises(toolchain.ToolchainError):
        toolchain.find_codon()


def test_supported_version():
    assert toolchain.is_supported_version("0.19.3")
    assert not toolchain.is_supported_version("0.18.2")
    assert not toolchain.is_supported_version("0.190.0")


def test_environment(launcher):
    result = launcher("env")
    assert result.returncode == 0, result.stderr
    assert "CODON_PATH=" in result.stdout and "TYPESAFE_CARLA_LIB=" in result.stdout


def test_info(launcher):
    result = launcher("info")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "native ABI         1.1" in result.stdout
    assert "carla ref" in result.stdout
