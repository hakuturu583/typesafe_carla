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
    assert tc_version.split(".post")[0] == codon
    assert toolchain.is_supported_version(codon)
