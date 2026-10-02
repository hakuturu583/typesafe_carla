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
import signal
import subprocess
import sys
import tempfile

from . import __version__, paths, toolchain

_LINKER_FLAGS = ("-linker-flags", "--linker-flags")

ENV_STRICT = "TYPESAFE_CARLA_STRICT"
ENV_COMPAT_WARNINGS = "TYPESAFE_CARLA_COMPAT_WARNINGS"
STRICT_FLAG = "--strict"
# The `what` literals of compat_warning("[tsc-compat] ...", ...) (codon/typesafe_carla/_strict.codon).
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


def _compile_with_scan(codon: str, build_args: list[str], scan_args: list[str] | None,
                       ir: str, env: dict[str, str]) -> int:
    """Runs `codon <build_args>` while an IR pass (`scan_args`, writing `ir`) runs in
    parallel, then prints the compile-time warnings. Returns the build's exit code.

    Best effort: if the IR pass fails, the real build reports any errors.
    """
    scan = None
    if scan_args is not None:
        try:
            scan = subprocess.Popen([codon] + scan_args, env=env, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        except OSError:
            scan = None
    rc = subprocess.run([codon] + build_args, env=env).returncode
    if scan is not None:
        if rc != 0:
            scan.kill()
        if scan.wait() == 0 and rc == 0:
            try:
                with open(ir, "rb") as f:
                    print_compat_warnings(f.read())
            except OSError:
                pass
    return rc


def _llvm_output(args: list[str]) -> str | None:
    """The .ll file a `codon build --llvm -o X` command writes, if that is what it is."""
    if not any(a in ("-llvm", "--llvm") for a in args):
        return None
    for i, arg in enumerate(args):
        if arg in ("-o", "--o") and i + 1 < len(args):
            return args[i + 1]
        if arg.startswith(("-o=", "--o=")):
            return arg.split("=", 1)[1]
    return None


def _build_with_warnings(codon: str, args: list[str], env: dict[str, str]) -> int:
    """`codon build`, then the compile-time compatibility warnings.

    An LLVM build is scanned directly (one compilation); otherwise the IR pass
    runs in parallel with the real build.
    """
    ll = _llvm_output(args)
    if ll is not None:
        return _scan_own_output(codon, args, ll, env)
    with tempfile.TemporaryDirectory(prefix="typesafe-codon-") as tmp:
        ir = os.path.join(tmp, "program.ll")
        return _compile_with_scan(codon, args, llvm_args(args, ir), ir, env)


def _scan_own_output(codon: str, args: list[str], ll: str, env: dict[str, str]) -> int:
    rc = subprocess.run([codon] + args, env=env).returncode
    if rc == 0:
        try:
            with open(ll, "rb") as f:
                print_compat_warnings(f.read())
        except OSError:
            pass
    return rc


def _run_with_warnings(codon: str, args: list[str], env: dict[str, str]) -> int | None:
    """`codon run`, with the compile-time compatibility warnings printed first.

    The warnings must come before the program's output, and the IR pass must
    not double the compile time, so the program is built into a temporary
    executable while the IR pass runs in parallel, then executed. None if
    the source file cannot be identified (the caller then runs it plainly).
    """
    src = _source_index(args)
    if src is None:
        return None
    with tempfile.TemporaryDirectory(prefix="typesafe-codon-") as tmp:
        ir = os.path.join(tmp, "program.ll")
        exe = os.path.join(tmp, os.path.splitext(os.path.basename(args[src]))[0] or "program")
        build = ["build"] + args[1:src] + ["-o", exe, args[src]]
        rc = _compile_with_scan(codon, build, llvm_args(args, ir), ir, env)
        if rc != 0:
            return rc
        sys.stderr.flush()
        previous = signal.signal(signal.SIGINT, signal.SIG_IGN)  # the program handles Ctrl-C
        try:
            rc = subprocess.run([exe] + args[src + 1:], env=env).returncode
        finally:
            signal.signal(signal.SIGINT, previous)
        return 128 - rc if rc < 0 else rc

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
