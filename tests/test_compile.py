"""Compile-pass and compile-fail tests (design section 32).

tests/compile/pass/*.codon must compile. tests/compile/fail/*.codon must fail
to compile, with the message given by the file's `# expect-error:` line, so a
program that fails for an unrelated reason (a typo, a missing import) does
not count as a pass. Programs are compiled to LLVM IR only; never run.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

COMPILE_DIR = Path(__file__).resolve().parent / "compile"
PASS = sorted((COMPILE_DIR / "pass").glob("*.codon"))
FAIL = sorted((COMPILE_DIR / "fail").glob("*.codon"))
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def _compile(launcher, source: Path, tmp_path: Path):
    return launcher("build", "--llvm", "-o", str(tmp_path / (source.stem + ".ll")), str(source))


def expected_error(source: Path) -> str:
    for line in source.read_text().splitlines():
        if line.startswith("# expect-error:"):
            return line.split(":", 1)[1].strip()
    raise AssertionError(f"{source.name} has no '# expect-error:' line")


def test_suites_are_not_empty():
    assert PASS and FAIL


@pytest.mark.parametrize("source", PASS, ids=lambda p: p.stem)
def test_compile_pass(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path)
    assert result.returncode == 0, ANSI.sub("", result.stderr)


@pytest.mark.parametrize("source", FAIL, ids=lambda p: p.stem)
def test_compile_fail(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path)
    stderr = ANSI.sub("", result.stderr)
    assert result.returncode != 0, f"{source.name} compiled but must not"
    assert expected_error(source) in stderr, stderr
