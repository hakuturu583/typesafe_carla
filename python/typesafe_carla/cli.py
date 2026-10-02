"""``typesafe-codon``: runs the Codon compiler with typesafe_carla configured.

    typesafe-codon run [-release] main.py      # compile and run
    typesafe-codon build [-release] main.py    # produce an executable
    typesafe-codon info                        # show what was found
    typesafe-codon env                         # print the environment as shell exports

Every other argument is passed to ``codon`` unchanged. The launcher sets:

* ``CODON_PATH``: prepends the typesafe_carla Codon sources;
* ``TYPESAFE_CARLA_LIB``: the native library the Codon module loads;
* ``CODON_DIR``, ``LD_LIBRARY_PATH``: for the Codon runtime.

For ``build``, the executable also gets an RPATH to the native library's
directory, so it runs without the launcher.
"""

from __future__ import annotations

import os
import sys

from . import __version__, paths, toolchain

_LINKER_FLAGS = ("-linker-flags", "--linker-flags")


def _prepend(value: str, existing: str | None) -> str:
    return value if not existing else value + os.pathsep + existing


def build_environment(tc: toolchain.Toolchain) -> dict[str, str]:
    env = os.environ.copy()
    lib = paths.native_library()
    env["CODON_DIR"] = str(tc.codon_dir)
    env["CODON_PATH"] = _prepend(str(paths.codon_modules_dir()), env.get("CODON_PATH"))
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


def _info(tc: toolchain.Toolchain | None, error: str | None) -> int:
    print(f"typesafe_carla     {__version__}")
    if tc is not None:
        version = tc.version()
        note = "" if toolchain.is_supported_version(version) else \
            f"  (untested; supported: {toolchain.SUPPORTED_CODON_SERIES}.x)"
        print(f"codon              {tc.executable} [{tc.source}] {version}{note}")
    else:
        print(f"codon              NOT FOUND: {error}")
    try:
        print(f"codon modules      {paths.codon_modules_dir()}")
    except paths.PathError as e:
        print(f"codon modules      NOT FOUND: {e}")
    try:
        lib = paths.native_library()
    except paths.PathError as e:
        print(f"native library     NOT FOUND: {e}")
        return 1
    print(f"native library     {lib}")
    import ctypes

    try:
        so = ctypes.CDLL(str(lib))
    except OSError as e:
        print(f"                   cannot be loaded: {e}")
        return 1
    for name in ("tsc_backend_name", "tsc_libcarla_version", "tsc_build_commit"):
        getattr(so, name).restype = ctypes.c_char_p
    abi = so.tsc_abi_version()
    print(f"native ABI         {abi >> 16}.{abi & 0xFFFF}")
    print(f"backend            {so.tsc_backend_name().decode()}")
    print(f"libcarla           {so.tsc_libcarla_version().decode()}")
    print(f"build commit       {so.tsc_build_commit().decode()}")
    return 0 if tc is not None else 1


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0

    try:
        tc = toolchain.find_codon()
    except toolchain.ToolchainError as e:
        if args[0] == "info":
            return _info(None, str(e))
        print(f"typesafe-codon: {e}", file=sys.stderr)
        return 2

    if args[0] == "info":
        return _info(tc, None)

    try:
        env = build_environment(tc)
    except paths.PathError as e:
        print(f"typesafe-codon: {e}", file=sys.stderr)
        return 2

    if args[0] == "env":
        for key in ("CODON_DIR", "CODON_PATH", paths.ENV_LIB, "LD_LIBRARY_PATH"):
            print(f"export {key}={env[key]!r}")
        return 0

    if args[0] == "build":
        args = add_rpath(args, str(paths.native_library().parent))

    cmd = [str(tc.executable)] + args
    sys.stdout.flush()
    os.execve(cmd[0], cmd, env)
    return 127  # not reached


if __name__ == "__main__":
    sys.exit(main())
