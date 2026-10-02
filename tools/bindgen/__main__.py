"""python -m tools.bindgen {generate [--check] | validate | coverage}"""

from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

from . import emit, spec as spec_mod
from .spec import ROOT


def generate(check: bool) -> int:
    spec = spec_mod.load()
    stale = []
    for path, text in emit.outputs(spec).items():
        old = path.read_text() if path.exists() else ""
        if old == text:
            continue
        stale.append(path)
        if check:
            sys.stdout.writelines(difflib.unified_diff(
                old.splitlines(True), text.splitlines(True),
                str(path.relative_to(ROOT)), f"{path.relative_to(ROOT)} (generated)"))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    if check and stale:
        print(f"\n{len(stale)} generated file(s) out of date; run: {emit.REGENERATE}", file=sys.stderr)
        return 1
    verb = "up to date" if check else f"wrote {len(stale)} file(s)"
    print(f"bindgen: {len(spec.functions)} functions, {verb}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.bindgen", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    g = sub.add_parser("generate", help="write the generated code (bindings/*.yaml -> C, C++, Codon)")
    g.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    for name, text in (("validate", "check the spec against the LibCarla headers of a build"),
                       ("coverage", "report which LibCarla methods the shim calls")):
        p = sub.add_parser(name, help=text)
        p.add_argument("--build-dir", type=Path, required=True,
                       help="a configured CMake build directory (uses its compile_commands.json)")
        if name == "coverage":
            p.add_argument("-o", "--output", type=Path, help="write Markdown here instead of stdout")
    args = parser.parse_args(argv)
    if args.command == "generate":
        return generate(args.check)
    from . import clang  # needs libclang; generate does not

    if args.command == "validate":
        return clang.validate(spec_mod.load(), args.build_dir)
    return clang.coverage(spec_mod.load(), args.build_dir, args.output)


if __name__ == "__main__":
    sys.exit(main())
