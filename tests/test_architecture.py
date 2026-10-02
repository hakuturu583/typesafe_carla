"""Architecture rules that must hold for every change (design sections 3, 19)."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODON_SOURCES = sorted((ROOT / "codon" / "typesafe_carla").glob("*.codon"))


def test_codon_sources_exist():
    assert (ROOT / "codon" / "typesafe_carla" / "__init__.codon") in CODON_SOURCES


def test_no_python_interop_in_core():
    """No CPython fallback: `from python import ...`, pyobj and @python are banned."""
    banned = re.compile(r"^\s*(from\s+python\s+import|import\s+python\b)|\bpyobj\b|@python\b",
                        re.MULTILINE)
    offenders = [p.name for p in CODON_SOURCES if banned.search(p.read_text())]
    assert not offenders, f"Python interop in core library: {offenders}"


def test_ffi_declarations_match_header():
    """Every function _ffi.codon imports is declared in ffi.h."""
    header = (ROOT / "native" / "include" / "typesafe_carla" / "ffi.h").read_text()
    declared = set(re.findall(r"\b(tsc_\w+)\s*\(", header))
    ffi = (ROOT / "codon" / "typesafe_carla" / "_ffi.codon").read_text()
    imported = set(re.findall(r"from C import LIB\.(tsc_\w+)\(", ffi))
    assert imported, "no C imports found"
    missing = imported - declared
    assert not missing, f"imported but not in ffi.h: {sorted(missing)}"


def test_ffi_structs_cross_by_pointer():
    """Codon passes an @tuple argument as separate scalars, which does not match
    the C calling convention for a struct passed by value. Structs cross as Ptr."""
    ffi = (ROOT / "codon" / "typesafe_carla" / "_ffi.codon").read_text()
    structs = set(re.findall(r"^@tuple\s*\nclass (\w+)", ffi, re.MULTILINE))
    assert "CColor" in structs
    offenders = []
    for name, args in re.findall(r"^from C import LIB\.(tsc_\w+)\(([^)]*)\)", ffi, re.MULTILINE):
        by_value = [a.strip() for a in args.split(",") if a.strip() in structs]
        if by_value:
            offenders.append((name, by_value))
    assert not offenders, f"structs passed by value across the C ABI: {offenders}"


def test_abi_version_matches_header():
    header = (ROOT / "native" / "include" / "typesafe_carla" / "ffi.h").read_text()
    ffi = (ROOT / "codon" / "typesafe_carla" / "_ffi.codon").read_text()
    for part in ("MAJOR", "MINOR"):
        h = re.search(rf"#define TSC_ABI_VERSION_{part} (\d+)", header).group(1)
        c = re.search(rf"^TSC_ABI_VERSION_{part} = (\d+)", ffi, re.MULTILINE).group(1)
        assert h == c, f"ABI {part}: ffi.h={h} _ffi.codon={c}"


def test_versions_agree():
    """One version for the Python package, the Codon module and CMake."""
    py = re.search(r'^__version__ = "(.+)"', (ROOT / "python" / "typesafe_carla" / "__init__.py")
                   .read_text(), re.MULTILINE).group(1)
    codon = re.search(r'^__version__ = "(.+)"', (ROOT / "codon" / "typesafe_carla" /
                                                  "__init__.codon").read_text(),
                      re.MULTILINE).group(1)
    assert py == codon


def test_toolchain_matches_supported_codon():
    from typesafe_carla import toolchain

    tc_pyproject = (ROOT / "toolchain" / "pyproject.toml").read_text()
    tc_version = re.search(r'^version = "(.+)"', tc_pyproject, re.MULTILINE).group(1)
    hook = (ROOT / "toolchain" / "hatch_build.py").read_text()
    codon = re.search(r'^CODON_VERSION = "(.+)"', hook, re.MULTILINE).group(1)
    package = (ROOT / "toolchain" / "src" / "typesafe_carla_toolchain" / "__init__.py").read_text()
    assert re.search(r'^CODON_VERSION = "(.+)"', package, re.MULTILINE).group(1) == codon
    assert tc_version.split(".post")[0] == codon
    assert toolchain.is_supported_version(codon)


def test_generated_bindings_up_to_date():
    """The code generated from bindings/*.yaml matches the spec
    (regenerate with `uv run python -m tools.bindgen generate`)."""
    from tools.bindgen import emit, spec

    stale = [str(path.relative_to(ROOT)) for path in emit.stale(spec.load())]
    assert not stale, f"out of date: {stale}; run `{emit.REGENERATE}`"


def test_generated_functions_are_not_hand_written():
    """A generated function has exactly one definition: the generated one."""
    from tools.bindgen import spec

    names = {f.name for f in spec.load().functions}
    hand_written = ROOT / "native" / "src"
    duplicates = [f"{p.name}: {name}" for p in hand_written.glob("*.cpp")
                  for name in re.findall(r"^tsc_status_t (tsc_\w+)\(", p.read_text(), re.MULTILINE)
                  if name in names]
    assert not duplicates, duplicates



def actor_shortcuts() -> list[tuple[str, str, str, str]]:
    """(name, parameters, first statement, second statement) of each public
    method _actor_compat.codon adds to Actor (docstrings skipped)."""
    compat = (ROOT / "codon" / "typesafe_carla" / "_actor_compat.codon").read_text()
    actor_block = compat.split("@extend\nclass Actor:", 1)[1]
    methods = re.findall(r"^    def (\w+)\((.*?)\).*?:\n(?:        \"\"\"[\s\S]*?\"\"\"\n)?(.*)\n(.*)\n",
                         actor_block, re.MULTILINE)
    return [m for m in methods if not m[0].startswith("_")]


def test_actor_shortcuts_reject_subclasses_and_are_marked():
    """Every Actor compatibility shortcut is a compile error on typed subclasses,
    then a strict-mode error / warning through compat_shortcut.

    Subclasses inherit methods added to Actor; only a generic `self: S` lets
    _plain_actor_only() see the real static type and reject it.
    """
    shortcuts = actor_shortcuts()
    assert len(shortcuts) == 34
    for name, params, first, second in shortcuts:
        assert params.startswith("self: S") and "S: type" in params, name
        assert first.strip().startswith(f'_plain_actor_only(self, "{name}'), name
        assert second.strip().startswith(
            f'compat_shortcut("Actor.{name}", "[tsc-compat] Actor.{name}'), name
