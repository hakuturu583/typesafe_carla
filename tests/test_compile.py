"""Compile-pass and compile-fail tests (design section 32).

tests/compile/pass/*.codon must compile; those with a `# strict` line must
also compile in strict mode (`typesafe-codon --strict`). tests/compile/fail/*.codon
must fail to compile, with the message given by the file's `# expect-error:`
line, so a program that fails for an unrelated reason (a typo, a missing
import) does not count as a pass. tests/compile/strict_fail/*.codon use a
Python-API compatibility shortcut: they must compile normally and fail in
strict mode with their `# expect-error:` message. Programs are compiled to
LLVM IR only; never run.
"""

from __future__ import annotations

from pathlib import Path

import pytest

COMPILE_DIR = Path(__file__).resolve().parent / "compile"
PASS = sorted((COMPILE_DIR / "pass").glob("*.codon"))
FAIL = sorted((COMPILE_DIR / "fail").glob("*.codon"))
STRICT_PASS = [p for p in PASS if "# strict" in p.read_text().splitlines()]
STRICT_FAIL = sorted((COMPILE_DIR / "strict_fail").glob("*.codon"))


def _compile(launcher, source: Path, tmp_path: Path, strict: bool = False):
    flags = ["--strict"] if strict else []
    return launcher(*flags, "build", "--llvm", "-o", str(tmp_path / (source.stem + ".ll")),
                    str(source))


def expected_error(source: Path) -> str:
    for line in source.read_text().splitlines():
        if line.startswith("# expect-error:"):
            return line.split(":", 1)[1].strip()
    raise AssertionError(f"{source.name} has no '# expect-error:' line")


def test_suites_are_not_empty():
    assert PASS and FAIL and STRICT_PASS and STRICT_FAIL


@pytest.mark.parametrize("source", PASS, ids=lambda p: p.stem)
def test_compile_pass(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("source", FAIL, ids=lambda p: p.stem)
def test_compile_fail(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path)
    assert result.returncode != 0, f"{source.name} compiled but must not"
    assert expected_error(source) in result.stderr, result.stderr


@pytest.mark.parametrize("source", STRICT_PASS, ids=lambda p: p.stem)
def test_compile_pass_strict(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path, strict=True)
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("source", STRICT_FAIL, ids=lambda p: p.stem)
def test_compile_fail_strict(launcher, source, tmp_path):
    result = _compile(launcher, source, tmp_path)
    assert result.returncode == 0, f"{source.name} must compile outside strict mode\n{result.stderr}"
    result = _compile(launcher, source, tmp_path, strict=True)
    assert result.returncode != 0, f"{source.name} compiled in strict mode but must not"
    assert expected_error(source) in result.stderr, result.stderr


def test_every_shortcut_warns_and_fails_in_strict_mode(launcher, tmp_path):
    """pass/actor_compat.codon uses every Actor shortcut: one compile-time warning
    per shortcut definition (the two apply_control overloads warn separately),
    and a compile error in strict mode."""
    from test_architecture import actor_shortcuts

    source = COMPILE_DIR / "pass" / "actor_compat.codon"
    result = _compile(launcher, source, tmp_path)
    assert result.returncode == 0, result.stderr
    warnings = {line for line in result.stderr.splitlines()
                if line.startswith("typesafe-codon: compile-time warning: ")}
    assert len(warnings) == len(actor_shortcuts()) == 61, "\n".join(sorted(warnings))
    result = _compile(launcher, source, tmp_path, strict=True)
    assert result.returncode != 0
    assert "strict mode: " in result.stderr, result.stderr


def test_compatibility_scenario_builds(launcher, tmp_path):
    """The compatibility scenario needs a server to run, but it must build:
    Codon can crash on constructs that type-check (see CLAUDE.md)."""
    source = Path(__file__).resolve().parent / "compatibility" / "scenario_typesafe.codon"
    result = launcher("build", "-o", str(tmp_path / "scenario"), str(source), timeout=900)
    assert result.returncode == 0, f"rc {result.returncode}\n{result.stderr}"
