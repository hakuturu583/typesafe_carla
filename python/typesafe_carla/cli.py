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
while the IR pass runs in parallel, and runs it as a child with argv[0] set
to the source path. The launcher forwards SIGTERM, SIGHUP, SIGQUIT and SIGINT
sent to it, kills and reaps its children on any exit, and reports a program
killed by signal N as exit status 128 + N. Building needs g++ (Codon links
with it): without g++, or if the scratch executable cannot be executed
(noexec), ``run`` falls back to ``codon run`` (JIT), with the warnings from a
separate IR compile. With warnings off or in strict mode, ``run`` execs
``codon run`` directly. ``TYPESAFE_CARLA_LAUNCHER_DEBUG=1`` reports why
compile-time warnings could not be produced.

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
# Signals the launcher passes on to the program (and to compilers it waits for).
_FORWARDED = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)


def _debug_enabled() -> bool:
    return os.environ.get(ENV_DEBUG, "0") not in ("", "0")


def _debug(message: str) -> None:
    if _debug_enabled():
        print(f"typesafe-codon: debug: {message}", file=sys.stderr)


def _unblock_signals() -> None:  # in the child, before exec: start with an empty mask
    signal.pthread_sigmask(signal.SIG_SETMASK, [])


class _Supervisor:
    """Runs child processes so that the launcher's own termination reaches them.

    The forwarded signals are blocked in the launcher and collected with
    sigtimedwait while it waits. A signal sent to the launcher by another
    process (kill, timeout, CI) is passed on to the running children; one the
    kernel generated (Ctrl-C at the terminal, hangup) already went to the whole
    foreground process group, children included, so it is not sent twice.
    Children start with SIG_DFL dispositions and an empty signal mask. On exit
    every child still running is killed and reaped, so nothing is orphaned.
    """

    def __init__(self) -> None:
        self.procs: list[subprocess.Popen] = []
        self.signalled: int | None = None

    def __enter__(self) -> "_Supervisor":
        signal.pthread_sigmask(signal.SIG_BLOCK, _FORWARDED)
        return self

    def __exit__(self, *exc) -> None:
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        # Drop signals still pending (e.g. the Ctrl-C that also ended the
        # program) so that unblocking does not kill the launcher afterwards.
        while signal.sigtimedwait(_FORWARDED, 0) is not None:
            pass
        signal.pthread_sigmask(signal.SIG_UNBLOCK, _FORWARDED)

    def popen(self, cmd: list[str], **kwargs) -> subprocess.Popen:
        p = subprocess.Popen(cmd, preexec_fn=_unblock_signals, **kwargs)
        self.procs.append(p)
        return p

    def wait(self, proc: subprocess.Popen) -> int:
        """Waits for `proc`, forwarding signals to all running children."""
        while proc.poll() is None:
            info = signal.sigtimedwait(_FORWARDED, 0.05)
            if info is None:
                continue
            self.signalled = info.si_signo
            if info.si_code <= 0:  # SI_USER, SI_QUEUE, SI_TKILL: sent by a process
                for p in self.procs:
                    if p.poll() is None:
                        p.send_signal(info.si_signo)
        return proc.returncode


def _exit_code(rc: int) -> int:
    """A child killed by signal N is reported as 128 + N, as shells do."""
    return 128 - rc if rc < 0 else rc


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
            return _exit_code(rc)
        tmp = paths.scratch_dir()
        try:
            ir = os.path.join(tmp, "program.ll")
            return _exit_code(_compile_with_scan(sup, codon, args, llvm_args(args, ir), ir, env))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


def _scan_only(codon: str, args: list[str], env: dict[str, str]) -> None:
    """Prints the compile-time warnings of `args`' program (a separate IR compile)."""
    with _Supervisor() as sup:
        tmp = paths.scratch_dir()
        try:
            ir = os.path.join(tmp, "program.ll")
            cmd = llvm_args(args, ir)
            if cmd is None:
                return
            rc = sup.wait(sup.popen([codon] + cmd, env=env, stdout=subprocess.DEVNULL,
                                    stderr=None if _debug_enabled() else subprocess.DEVNULL))
            if rc == 0:
                _print_compat_warnings_from(ir)
            else:
                _debug(f"compatibility scan failed (exit {rc}); no compile-time warnings")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


def _run_with_warnings(codon: str, args: list[str], env: dict[str, str]) -> int | None:
    """`codon run`, with the compile-time compatibility warnings printed first.

    The warnings must come before the program's output, and the IR pass must
    not double the compile time, so the program is built into a temporary
    executable (in the cache directory) while the IR pass runs in parallel,
    then executed with argv[0] set to the source path, as `codon run` does.
    The launcher stays the program's parent: it forwards termination signals
    and reports a program killed by signal N as exit status 128 + N.

    Building needs g++ (Codon links with it) and an executable scratch
    directory. Without g++ the warnings come from a separate IR compile and
    the program runs with `codon run` (JIT), costing a second compilation; if
    the built program cannot be executed (noexec), it also falls back to
    `codon run`. None means: exec `codon run` (also when the source file
    cannot be identified, then without compile-time warnings).
    """
    src = _source_index(args)
    if src is None:
        return None
    if shutil.which("g++", path=env.get("PATH")) is None:
        _debug("g++ not found: separate compatibility scan, then codon run (JIT)")
        _scan_only(codon, args, env)
        return None
    with _Supervisor() as sup:
        tmp = paths.scratch_dir()
        try:
            ir = os.path.join(tmp, "program.ll")
            exe = os.path.join(tmp, "program")
            build = ["build"] + args[1:src] + ["-o", exe, args[src]]
            rc = _compile_with_scan(sup, codon, build, llvm_args(args, ir), ir, env)
            if rc != 0 or sup.signalled is not None:
                return _exit_code(rc) if rc != 0 else 128 + sup.signalled
            sys.stdout.flush()
            sys.stderr.flush()
            try:
                program = sup.popen([args[src]] + args[src + 1:], executable=exe, env=env)
            except OSError as e:
                _debug(f"cannot execute the built program ({e}): codon run (JIT)")
                return None
            return _exit_code(sup.wait(program))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


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
    os.execve(cmd[0], cmd, env)
    return 127  # not reached


if __name__ == "__main__":
    sys.exit(main())
