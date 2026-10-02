"""``typesafe-codon``: runs the Codon compiler with typesafe_carla configured.

    typesafe-codon [--strict] run [-release] main.py      # compile and run
    typesafe-codon [--strict] build [-release] main.py    # produce an executable
    typesafe-codon info                                   # show what was found
    typesafe-codon env                                    # print the environment as shell exports

Every other argument is passed to ``codon`` unchanged. ``--strict`` (also
accepted right after ``run``/``build``) or ``TYPESAFE_CARLA_STRICT=1`` turns on
strict mode: the Python-API compatibility shortcuts become compile errors.
Otherwise ``run`` and ``build`` print a compile-time warning for each
shortcut the program uses (``TYPESAFE_CARLA_COMPAT_WARNINGS=0`` silences
them, and the matching run-time warnings).

To print those before the program's output without compiling twice, ``run``
builds the program into a scratch executable (under the cache directory)
while the IR pass runs in parallel, then execs it with argv[0] set to the
source path: the launcher process becomes the program (same PID, signals and
exit status). During the compile the launcher passes SIGINT, SIGTERM, SIGHUP
and SIGQUIT on to the compilers, kills them, and then dies of that signal
itself. Building needs g++ (Codon links with it): without g++, or if the
scratch executable cannot be executed (noexec), ``run`` falls back to
``codon run`` (JIT), with the warnings from a separate IR compile. With
warnings off or in strict mode, ``run`` execs ``codon run`` directly.
``TYPESAFE_CARLA_LAUNCHER_DEBUG=1`` reports why compile-time warnings could
not be produced or a fallback was taken.

The launcher sets:

* ``CODON_PATH``: a generated directory with the typesafe_carla Codon sources
  and the build configuration (``paths.codon_path_dir``);
* ``TYPESAFE_CARLA_LIB``: the native library the Codon module loads;
* ``CODON_DIR``, ``LD_LIBRARY_PATH``: for the Codon runtime.

For ``build``, the executable also gets an RPATH to the native library's
directory, so it runs without the launcher.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import signal
import subprocess
import sys

from . import __version__, paths, toolchain

_LINKER_FLAGS = ("-linker-flags", "--linker-flags")

ENV_STRICT = "TYPESAFE_CARLA_STRICT"
ENV_COMPAT_WARNINGS = "TYPESAFE_CARLA_COMPAT_WARNINGS"
STRICT_FLAG = "--strict"
# The `what` literals of compat_shortcut(name, "[tsc-compat] ...", ...) (codon/typesafe_carla/_strict.codon).
# String constants only (`[N x i8] c"...\00"`): Literal[str] arguments also end up in
# mangled function names, which must not count.
_COMPAT_RE = re.compile(rb'c"\[tsc-compat\] ([^"]*)\\00"')
_IR_ESCAPE = re.compile(rb"\\([0-9A-Fa-f]{2})")  # LLVM c"..." escapes: \22 is '"'
_SOURCE_SUFFIXES = (".py", ".codon", ".seq")
_OUTPUT_KINDS = ("-exe", "--exe", "-lib", "--lib", "-obj", "--obj", "-llvm", "--llvm", "-pyext",
                 "--pyext")


def _prepend(value: str, existing: str | None) -> str:
    return value if not existing else value + os.pathsep + existing


def strict_from_env() -> bool:
    return os.environ.get(ENV_STRICT, "0") not in ("", "0")


def compat_warnings_enabled() -> bool:
    return os.environ.get(ENV_COMPAT_WARNINGS, "1") != "0"


def build_environment(tc: toolchain.Toolchain, strict: bool = False) -> dict[str, str]:
    env = os.environ.copy()
    lib = paths.native_library()
    env["CODON_DIR"] = str(tc.codon_dir)
    env["CODON_PATH"] = _prepend(str(paths.codon_path_dir(strict)), env.get("CODON_PATH"))
    env[paths.ENV_LIB] = str(lib)
    ld = [str(lib.parent)] + [str(d) for d in tc.library_dirs()]
    env["LD_LIBRARY_PATH"] = _prepend(os.pathsep.join(ld), env.get("LD_LIBRARY_PATH"))
    return env


def add_rpath(args: list[str], directory: str) -> list[str]:
    """Adds an RPATH for `directory` to a `codon build ...` argument list.

    DT_RPATH (not RUNPATH) so that dlopen() calls made from Codon's runtime
    library also search it.
    """
    flag = f"-Wl,--disable-new-dtags,-rpath,{directory}"
    out = list(args)
    for i, arg in enumerate(out):
        for name in _LINKER_FLAGS:
            if arg == name and i + 1 < len(out):
                out[i + 1] = f"{out[i + 1]} {flag}"
                return out
            if arg.startswith(name + "="):
                out[i] = f"{arg} {flag}"
                return out
    return [out[0], f"--linker-flags={flag}"] + out[1:]


def split_strict(args: list[str]) -> tuple[list[str], bool]:
    """Removes the launcher's --strict flag (before or right after the subcommand)."""
    out = list(args)
    strict = False
    if out and out[0] == STRICT_FLAG:
        out.pop(0)
        strict = True
    if len(out) > 1 and out[0] in ("run", "build") and out[1] == STRICT_FLAG:
        out.pop(1)
        strict = True
    return out, strict


def _source_index(args: list[str]) -> int | None:
    """Index of the program source in a `codon run/build ...` argument list."""
    for i, arg in enumerate(args[1:], start=1):
        if not arg.startswith("-") and arg.endswith(_SOURCE_SUFFIXES) and os.path.isfile(arg):
            return i
    return None


def llvm_args(args: list[str], output: str) -> list[str] | None:
    """`codon build --llvm` arguments compiling the same program as `args` into `output`.

    Program arguments (after the source) and output options are dropped.
    None if the source file cannot be found.
    """
    src = _source_index(args)
    if src is None:
        return None
    opts: list[str] = []
    skip = False
    for arg in args[1:src]:
        if skip:
            skip = False
            continue
        if arg in ("-o", "--o") + _LINKER_FLAGS:
            skip = True
            continue
        if arg.startswith(("-o=", "--o=")) or arg.startswith(tuple(f + "=" for f in _LINKER_FLAGS)):
            continue
        if arg in _OUTPUT_KINDS or arg in ("-release", "--release"):
            continue  # the IR pass need not optimize
        opts.append(arg)
    return ["build", "--llvm", "-o", output] + opts + [args[src]]


def compat_paths_in_ir(ir: bytes) -> list[str]:
    """Distinct compatibility paths named in LLVM IR, in order of appearance."""
    seen: list[str] = []
    for m in _COMPAT_RE.finditer(ir):
        raw = _IR_ESCAPE.sub(lambda e: bytes([int(e.group(1), 16)]), m.group(1))
        what = raw.decode(errors="replace").strip()
        if what and what not in seen:
            seen.append(what)
    return seen


def print_compat_warnings(ir: bytes) -> None:
    for what in compat_paths_in_ir(ir):
        print(f"typesafe-codon: compile-time warning: {what}: Python-API compatibility path, "
              f"not statically checked (use {STRICT_FLAG} to make this an error)",
              file=sys.stderr)


def _print_compat_warnings_from(ir: str) -> None:
    try:
        with open(ir, "rb") as f:
            print_compat_warnings(f.read())
    except OSError:
        pass


ENV_DEBUG = "TYPESAFE_CARLA_LAUNCHER_DEBUG"
# Signals that end the launcher; while it waits for compilers they are passed on.
_FORWARDED = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)


def _debug_enabled() -> bool:
    return os.environ.get(ENV_DEBUG, "0") not in ("", "0")


def _debug(message: str) -> None:
    if _debug_enabled():
        print(f"typesafe-codon: debug: {message}", file=sys.stderr)


def _restore_signals() -> None:
    """Before exec: undo Python's SIG_IGN for SIGPIPE and SIGXFSZ, which an exec
    would otherwise pass on to the program (subprocess does the same)."""
    for name in ("SIGPIPE", "SIGXFSZ"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), signal.SIG_DFL)


def _unblock_signals() -> None:  # in the child, before exec: start with an empty mask
    signal.pthread_sigmask(signal.SIG_SETMASK, [])


class _Supervisor:
    """Runs the compilers (the build and the parallel IR pass) so that the
    launcher's own termination reaches them.

    The forwarded signals and SIGCHLD are blocked in the launcher and collected
    with sigtimedwait while it waits. A signal sent to the launcher alone (kill)
    is passed on to the running compilers; one also delivered to them (terminal
    Ctrl-C, a signal to the process group) is not sent twice. Compilers start
    with an empty signal mask. On exit every compiler still running is killed
    and reaped; `signalled` records the first terminating signal received, of
    which the caller then dies (`_die_of`).
    """

    def __init__(self) -> None:
        self.procs: list[subprocess.Popen] = []
        self.signalled: int | None = None

    def __enter__(self) -> "_Supervisor":
        signal.pthread_sigmask(signal.SIG_BLOCK, _FORWARDED + (signal.SIGCHLD,))
        return self

    def __exit__(self, *exc) -> None:
        self.reap()
        while True:  # collect what is still pending, so that unblocking does not act on it
            info = signal.sigtimedwait(_FORWARDED + (signal.SIGCHLD,), 0)
            if info is None:
                break
            if info.si_signo != signal.SIGCHLD and self.signalled is None:
                self.signalled = info.si_signo
        signal.pthread_sigmask(signal.SIG_UNBLOCK, _FORWARDED + (signal.SIGCHLD,))

    def reap(self) -> None:
        """Kills and waits for every compiler still running."""
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()

    def popen(self, cmd: list[str], **kwargs) -> subprocess.Popen:
        p = subprocess.Popen(cmd, preexec_fn=_unblock_signals, **kwargs)
        self.procs.append(p)
        return p

    def wait(self, proc: subprocess.Popen) -> int:
        """Waits for `proc`, passing terminating signals on to all running compilers."""
        while proc.poll() is None:
            info = signal.sigtimedwait(_FORWARDED + (signal.SIGCHLD,), 1.0)
            if info is None or info.si_signo == signal.SIGCHLD:
                continue
            if self.signalled is None:
                self.signalled = info.si_signo
            for p in self.procs:
                if p.poll() is None:
                    p.send_signal(info.si_signo)  # a second copy to a compiler is harmless
        return proc.returncode


def _die_of(sig: int) -> int:
    """Ends the launcher by signal `sig` (as its child did, or as it was asked to),
    so a shell sees death by signal and, for SIGINT, stops a loop too."""
    sys.stdout.flush()
    sys.stderr.flush()
    signal.signal(sig, signal.SIG_DFL)
    signal.pthread_sigmask(signal.SIG_UNBLOCK, [sig])
    os.kill(os.getpid(), sig)
    return 128 + sig  # not reached


def _finish(rc: int, sup: _Supervisor) -> int:
    """The launcher's exit for a compile that returned `rc` under `sup`."""
    if sup.signalled is not None:
        return _die_of(sup.signalled)
    if rc < 0:
        return _die_of(-rc)
    return rc


def _compile_with_scan(sup: _Supervisor, codon: str, build_args: list[str],
                       scan_args: list[str] | None, ir: str, env: dict[str, str]) -> int:
    """Runs `codon <build_args>` while an IR pass (`scan_args`, writing `ir`) runs in
    parallel, then prints the compile-time warnings. Returns the build's exit code.

    Best effort: if the IR pass fails, the real build reports any errors.
    """
    scan = None
    if scan_args is not None:
        try:
            scan = sup.popen([codon] + scan_args, env=env, stdout=subprocess.DEVNULL,
                             stderr=None if _debug_enabled() else subprocess.DEVNULL)
        except OSError as e:
            _debug(f"compatibility scan not started: {e}")
    rc = sup.wait(sup.popen([codon] + build_args, env=env))
    if scan is not None:
        if rc != 0 or sup.signalled is not None:
            scan.kill()
        scan_rc = sup.wait(scan)
        if scan_rc == 0 and rc == 0:
            _print_compat_warnings_from(ir)
        elif rc == 0:
            _debug(f"compatibility scan failed (exit {scan_rc}); no compile-time warnings")
    return rc


def _llvm_output(args: list[str]) -> str | None:
    """The .ll file a `codon build --llvm` command writes, if that is what it is."""
    if not any(a in ("-llvm", "--llvm") for a in args):
        return None
    for i, arg in enumerate(args):
        if arg in ("-o", "--o") and i + 1 < len(args):
            return args[i + 1]
        if arg.startswith(("-o=", "--o=")):
            return arg.split("=", 1)[1]
    src = _source_index(args)  # without -o: <source stem>.ll next to the source
    return None if src is None else os.path.splitext(args[src])[0] + ".ll"


def _build_with_warnings(codon: str, args: list[str], env: dict[str, str]) -> int:
    """`codon build`, then the compile-time compatibility warnings.

    An LLVM build is scanned directly (one compilation); otherwise the IR pass
    runs in parallel with the real build.
    """
    ll = _llvm_output(args)
    with _Supervisor() as sup:
        if ll is not None:
            rc = sup.wait(sup.popen([codon] + args, env=env))
            if rc == 0:
                _print_compat_warnings_from(ll)
        else:
            tmp = paths.scratch_dir()
            try:
                ir = os.path.join(tmp, "program.ll")
                rc = _compile_with_scan(sup, codon, args, llvm_args(args, ir), ir, env)
                sup.reap()
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
    return _finish(rc, sup)


def _scan_only(codon: str, args: list[str], env: dict[str, str]) -> None:
    """Prints the compile-time warnings of `args`' program (a separate IR compile)."""
    with _Supervisor() as sup:
        tmp = paths.scratch_dir()
        try:
            ir = os.path.join(tmp, "program.ll")
            cmd = llvm_args(args, ir)
            rc = 1
            if cmd is not None:
                rc = sup.wait(sup.popen([codon] + cmd, env=env, stdout=subprocess.DEVNULL,
                                        stderr=None if _debug_enabled() else subprocess.DEVNULL))
            if rc == 0:
                _print_compat_warnings_from(ir)
            else:
                _debug(f"compatibility scan failed (exit {rc}); no compile-time warnings")
            sup.reap()
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if sup.signalled is not None:
        _die_of(sup.signalled)


def _run_with_warnings(codon: str, args: list[str], env: dict[str, str]) -> int | None:
    """`codon run`, with the compile-time compatibility warnings printed first.

    The warnings must come before the program's output, and the IR pass must
    not double the compile time, so the program is built into a scratch
    executable (in the cache directory) while the IR pass runs in parallel.
    The launcher then opens the executable, removes the scratch directory and
    execs it with argv[0] set to the source path, as `codon run` does: the
    launcher becomes the program (same PID, signals and exit status).

    Building needs g++ (Codon links with it). Without g++ the warnings come
    from a separate IR compile and the program runs with `codon run` (JIT),
    costing a second compilation; if the built program cannot be executed
    (noexec), it also falls back to `codon run`. Returns an exit status if the
    compile failed, None for: exec `codon run` (also when the source file
    cannot be identified, then without compile-time warnings). On success it
    does not return.
    """
    src = _source_index(args)
    if src is None:
        return None
    if shutil.which("g++", path=env.get("PATH")) is None:
        _debug("g++ not found: separate compatibility scan, then codon run (JIT)")
        _scan_only(codon, args, env)
        return None
    fd = -1
    with _Supervisor() as sup:
        tmp = paths.scratch_dir()
        try:
            ir = os.path.join(tmp, "program.ll")
            exe = os.path.join(tmp, "program")
            build = ["build"] + args[1:src] + ["-o", exe, args[src]]
            rc = _compile_with_scan(sup, codon, build, llvm_args(args, ir), ir, env)
            sup.reap()
            if rc == 0 and sup.signalled is None:
                fd = os.open(exe, os.O_RDONLY)  # stays executable after the rmtree
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    # __exit__ collected any signal that arrived until now (e.g. a Ctrl-C between
    # the end of the compile and the exec): it is not lost.
    if fd < 0 or sup.signalled is not None:
        if fd >= 0:
            os.close(fd)
        return _finish(rc, sup)
    sys.stdout.flush()
    sys.stderr.flush()
    _restore_signals()
    try:
        os.execve(fd, [args[src]] + args[src + 1:], env)  # fexecve
    except OSError as e:
        os.close(fd)
        _debug(f"cannot execute the built program ({e}): codon run (JIT)")
    return None


def _info(tc: toolchain.Toolchain | None, error: str | None, strict: bool) -> int:
    print(f"typesafe_carla     {__version__}")
    print(f"strict mode        {'on' if strict else 'off'} ({STRICT_FLAG} or {ENV_STRICT}=1)")
    if tc is not None:
        version = tc.version()
        note = "" if toolchain.is_supported_version(version) else \
            f"  (untested; supported: {toolchain.SUPPORTED_CODON_SERIES}.x)"
        print(f"codon              {tc.executable} [{tc.source}] {version}{note}")
    else:
        print(f"codon              NOT FOUND: {error}")
    try:
        print(f"codon modules      {paths.codon_modules_dir()}")
        print(f"CODON_PATH         {paths.codon_path_dir(strict)}")
    except paths.PathError as e:
        print(f"codon modules      NOT FOUND: {e}")
    try:
        lib = paths.native_library()
    except paths.PathError as e:
        print(f"native library     NOT FOUND: {e}")
        return 1
    print(f"native library     {lib}")
    try:
        native = paths.native_info(lib)
    except OSError as e:
        print(f"                   cannot be loaded: {e}")
        return 1
    print(f"native ABI         {native['abi']}")
    print(f"backend            {native['backend']}")
    print(f"libcarla           {native['libcarla_version']}")
    print(f"carla ref          {native['carla_git_ref']}")
    print(f"carla commit       {native['carla_git_commit']}")
    print(f"build commit       {native['build_commit']}")
    return 0 if tc is not None else 1


def main(argv: list[str] | None = None) -> int:
    args, strict = split_strict(list(sys.argv[1:] if argv is None else argv))
    strict = strict or strict_from_env()
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0

    try:
        tc = toolchain.find_codon()
    except toolchain.ToolchainError as e:
        if args[0] == "info":
            return _info(None, str(e), strict)
        print(f"typesafe-codon: {e}", file=sys.stderr)
        return 2

    if args[0] == "info":
        return _info(tc, None, strict)

    try:
        env = build_environment(tc, strict)
    except paths.PathError as e:
        print(f"typesafe-codon: {e}", file=sys.stderr)
        return 2

    if args[0] == "env":
        for key in ("CODON_DIR", "CODON_PATH", paths.ENV_LIB, "LD_LIBRARY_PATH"):
            print(f"export {key}={shlex.quote(env[key])}")
        return 0

    codon = str(tc.executable)
    warn = not strict and compat_warnings_enabled() and args[0] in ("run", "build")
    if args[0] == "build":
        args = add_rpath(args, os.path.dirname(env[paths.ENV_LIB]))
        if warn:
            sys.stdout.flush()
            return _build_with_warnings(codon, args, env)
    elif warn:
        rc = _run_with_warnings(codon, args, env)
        if rc is not None:
            return rc

    cmd = [codon] + args
    sys.stdout.flush()
    sys.stderr.flush()
    _restore_signals()
    os.execve(cmd[0], cmd, env)
    return 127  # not reached


if __name__ == "__main__":
    sys.exit(main())
