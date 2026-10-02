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
    assert "native ABI         3.3" in result.stdout
    assert "carla ref" in result.stdout


def test_split_strict():
    assert cli.split_strict(["--strict", "run", "a.py"]) == (["run", "a.py"], True)
    assert cli.split_strict(["build", "--strict", "-release", "a.py"]) == \
        (["build", "-release", "a.py"], True)
    assert cli.split_strict(["run", "a.py", "--strict"]) == (["run", "a.py", "--strict"], False)
    assert cli.split_strict(["info"]) == (["info"], False)


def test_llvm_args(tmp_path):
    src = tmp_path / "main.py"
    src.write_text("")
    args = ["build", "--linker-flags=-lm", "-release", "-o", "out", "-exe", str(src)]
    assert cli.llvm_args(args, "x.ll") == ["build", "--llvm", "-o", "x.ll", str(src)]
    args = ["run", "-D", "N=1", str(src), "progarg", "-x"]
    assert cli.llvm_args(args, "x.ll") == ["build", "--llvm", "-o", "x.ll", "-D", "N=1", str(src)]
    assert cli.llvm_args(["run", str(tmp_path / "missing.py")], "x.ll") is None


def test_compat_paths_in_ir():
    ir = (b'@s0 = private constant [14 x i8] c"[tsc-compat] \\00"\n'
          b"declare void @\"f.0,'[tsc-compat] A.b() without as_b()'\"()\n"
          b'@s1 = private constant [36 x i8] c"[tsc-compat] A.b() without as_b()\\00"\n'
          b'@s2 = private constant [36 x i8] c"[tsc-compat] A.b() without as_b()\\00"\n'
          b'@s3 = private constant [30 x i8] c"[tsc-compat] say \\22hi\\22\\00"\n')
    assert cli.compat_paths_in_ir(ir) == ["A.b() without as_b()", 'say "hi"']
    assert cli.compat_paths_in_ir(b'declare void @"f.0,\'[tsc-compat] x\'"()') == []


def test_codon_path_dir(tmp_path, monkeypatch):
    monkeypatch.setenv(paths.ENV_CACHE_DIR, str(tmp_path))
    package = paths.codon_modules_dir() / "typesafe_carla"
    lax, strict = paths.codon_path_dir(False), paths.codon_path_dir(True)
    assert lax != strict and lax.parent == tmp_path / "codon-path"
    for directory, value in ((lax, 0), (strict, 1)):
        assert (directory / "typesafe_carla").resolve() == package.resolve()
        config = (directory / "_tsc_build_config.codon").read_text()
        assert f"TSC_STRICT: Static[int] = {value}" in config
    assert paths.codon_path_dir(False) == lax  # reused
    (lax / "_tsc_build_config.codon").write_text("stale")
    assert paths.codon_path_dir(False) == lax
    assert "= 0" in (lax / "_tsc_build_config.codon").read_text()
    assert sorted(p.name for p in lax.parent.iterdir()) == sorted([lax.name, strict.name])


def test_info_shows_strict_mode(launcher):
    assert "strict mode        off" in launcher("info").stdout
    assert "strict mode        on" in launcher("--strict", "info").stdout
    result = launcher("info", env={"TYPESAFE_CARLA_STRICT": "1"})
    assert "strict mode        on" in result.stdout
    assert "CODON_PATH" in result.stdout


_SLEEPER = """
import sys, time
from C import getpid() -> i32
for line in open("/proc/self/status"):  # (read() sees procfs files as empty)
    if line.startswith("SigIgn:") or line.startswith("SigBlk:"):
        print(line.strip())
print("argv0", sys.argv[0])
print("pid", getpid())
print("ready")
sys.stdout.flush()
time.sleep(60.0)
print("not interrupted")
"""


def _default_signals() -> None:
    """In the launcher child: SIG_DFL for SIGINT/SIGQUIT, which a shell ignores in
    background jobs (e.g. `pytest &`) and an exec would otherwise pass on."""
    import signal

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGQUIT, signal.SIG_DFL)


def _start_sleeper(launcher, tmp_path):
    """Starts `typesafe-codon run` on a program that sleeps; returns (proc, info, source)."""
    import os
    import subprocess
    import sys

    launcher("info")  # skips when Codon or the native library is missing
    source = tmp_path / "sleeper.codon"
    source.write_text(_SLEEPER)
    env = {**os.environ, paths.ENV_CACHE_DIR: str(tmp_path / "cache")}
    proc = subprocess.Popen([sys.executable, "-m", "typesafe_carla.cli", "run", str(source)],
                            stdout=subprocess.PIPE, text=True, env=env,
                            cwd=Path(__file__).resolve().parent.parent,
                            preexec_fn=_default_signals)
    info = {}
    for line in proc.stdout:
        key, _, value = line.strip().partition(" ") if not line.startswith("Sig") else \
            line.strip().partition(":")
        info[key] = value.strip()
        if key == "ready":
            break
    return proc, info, source


@pytest.mark.parametrize("sig", ["SIGINT", "SIGTERM"])
def test_run_execs_the_built_program(launcher, tmp_path, sig):
    """The launcher becomes the program: same PID, default signal handling,
    argv[0] = source, death by the signal, and no scratch directory left."""
    import signal

    proc, info, source = _start_sleeper(launcher, tmp_path)
    try:
        assert int(info["pid"]) == proc.pid
        assert int(info["SigIgn"], 16) & (1 << (signal.SIGINT - 1)) == 0, info
        assert int(info["SigBlk"], 16) == 0, info
        assert info["argv0"] == str(source)
        assert list((tmp_path / "cache" / "run").iterdir()) == []
        proc.send_signal(getattr(signal, sig))
        assert proc.wait(timeout=30) == -getattr(signal, sig)  # died of it
    finally:
        proc.kill()


def test_run_falls_back_to_jit_when_the_program_cannot_be_executed(launcher, tmp_path,
                                                                   monkeypatch):
    """noexec scratch directory: fexecve fails, so `codon run` is exec'd instead."""
    import os

    from typesafe_carla import cli

    launcher("info")  # skips when Codon or the native library is missing
    source = tmp_path / "hello.codon"
    source.write_text("print('hello')\n")
    monkeypatch.setenv(paths.ENV_CACHE_DIR, str(tmp_path / "cache"))
    calls = []

    class Exec(Exception):
        pass

    def fake_execve(path, argv, env):
        calls.append((path, list(argv)))
        if isinstance(path, int):
            raise PermissionError(13, "Permission denied")  # what noexec gives
        raise Exec

    monkeypatch.setattr(os, "execve", fake_execve)
    with pytest.raises(Exec):
        cli.main(["run", str(source)])
    assert isinstance(calls[0][0], int) and calls[0][1] == [str(source)]
    assert calls[1][1][1:] == ["run", str(source)]
    assert list((tmp_path / "cache" / "run").iterdir()) == []


def test_sweep_removes_old_scratch_dirs(tmp_path, monkeypatch):
    import os
    import time

    monkeypatch.setenv(paths.ENV_CACHE_DIR, str(tmp_path))
    old = tmp_path / "run" / "typesafe-codon-old"
    old.mkdir(parents=True)
    past = time.time() - paths.SCRATCH_MAX_AGE - 60
    os.utime(old, (past, past))
    fresh = paths.scratch_dir()
    assert not old.exists() and os.path.isdir(fresh)


def test_run_without_gpp_falls_back_to_jit(launcher, tmp_path):
    import os

    source = tmp_path / "compat.codon"
    source.write_text("import typesafe_carla as carla\n"
                      "a = carla.Client('localhost', 2000).get_world().get_actors()[0]\n"
                      "a.set_autopilot(False)\n"
                      "import sys\nprint('argv0', sys.argv[0])\n")
    empty = tmp_path / "bin"
    empty.mkdir()
    result = launcher("run", str(source), env={"PATH": str(empty)})
    if paths.native_info()["backend"] != "mock":
        pytest.skip("needs the mock backend")
    assert result.returncode == 0, result.stderr
    assert "compile-time warning: Actor.set_autopilot() without as_vehicle()" in result.stderr
    assert "typesafe_carla: warning: Actor.set_autopilot()" in result.stderr
    assert os.path.basename(result.stdout.split("argv0", 1)[1].strip()) == "compat.codon"
