"""Spec checks of the binding generator (tools/bindgen/spec.py): malformed
entries fail with SpecError instead of generating wrong code."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

from tools.bindgen import spec

ROOT = Path(__file__).resolve().parent.parent


def _load(tmp_path: Path, entry: str, types_extra: str = "") -> spec.Spec:
    shutil.copy(ROOT / "bindings" / "types.yaml", tmp_path / "types.yaml")
    if types_extra:
        with open(tmp_path / "types.yaml", "a") as f:
            f.write(types_extra)
    (tmp_path / "thing.yaml").write_text(
        "class: carla::client::Thing\nprefix: thing\n"
        "self: {type: tsc_thing_t, name: thing, get: thing_of}\n"
        "blocks:\n  thing:\n" + "".join(f"    {line}\n" for line in entry.strip().splitlines()))
    return spec.load(tmp_path)


def test_valid_entries_load(tmp_path):
    s = _load(tmp_path, """
f: {call: F, args: {name: string_in, n: bool}, out: string_list}
g: {call: G, args: {flag: bool}, optional: {name: Thing.g, missing_in: ["0.10.0"]}}
""")
    f, g = s.functions
    assert f.c_params() == ["tsc_thing_t *thing", "const char *name, size_t name_len",
                            "int32_t n", "tsc_string_list_t *out"]
    assert f.codon_params() == ["cobj", "cobj, int", "i32", "Ptr[CStringList]"]
    assert g.optional == "Thing.g" and g.missing_in == ("0.10.0",)
    assert g.body().startswith("TSC_CALL_OPTIONAL(thing_of(thing), G, \"Thing.g\"")



def test_via_checks_the_handle_before_the_arguments(tmp_path):
    """A `via` call takes the object as a function argument, and C++ evaluates
    function arguments in no fixed order: with other arguments, the object
    (the handle check) is bound first through a lambda, as in a member call,
    so a NULL handle is reported before an invalid argument (issue #19)."""
    s = _load(tmp_path, """
f: {call: F, via: f_compat, args: {name: string_in}, out: transform}
g: {call: G, via: g_compat, out: string_list}
""")
    f, g = s.functions
    assert ("[&](auto &self_) { return f_compat(self_, to_string(name, name_len, \"name\")); }"
            "(thing_of(thing))") in f.body()
    assert g.body() == "string_list_assign(out, g_compat(thing_of(thing)));"


@pytest.mark.parametrize("entry, message", [
    ("f: {call: F, out: bool, optional: {name: Thing.f, missing_in: [\"0.10.0\"]}}",
     "an optional method has no output"),
    ("f: {call: F, optional: Thing.f}", "optional must be"),
    ("f: {call: F, optional: {name: Thing.f, missing_in: []}}", "optional must be"),
    ("f: {call: F, optional: {name: Thing.f}}", "optional must be"),
    ("f: {call: F, out: string_in}", "is input-only"),
    ("f: {call: F, args: {s: string}}", "is output-only"),
    ("f: {call: F, out: vehicle}", "has no from_carla to create the output handle"),
    ("f: {call: F, via: f_compat, optional: {name: Thing.f, missing_in: [\"0.10.0\"]}}",
     "`optional` and `via` exclude each other"),
    ("f: {call: F, colour: red}", "unknown keys ['colour']"),
    ("f: {call: F, args: {x: no_such_type}}", "unknown type 'no_such_type'"),
    ("f: {call: F, args: {a: location_path, b: road_option_route}}",
     "duplicate C parameter(s) ['count']"),
    ("f: {call: F, args: {thing: bool}}", "duplicate C parameter(s) ['thing']"),
    ("f: {call: F, args: {name: string_in, name_len: bool}}", "duplicate C parameter(s) ['name_len']"),
])
def test_spec_errors(tmp_path, entry, message):
    with pytest.raises(spec.SpecError) as e:
        _load(tmp_path, entry)
    assert message in str(e.value)


def test_unknown_type_key(tmp_path):
    with pytest.raises(spec.SpecError, match=r"unknown keys \['colour'\]"):
        _load(tmp_path, "f: {call: F}", "odd:\n  c: int\n  codon: int\n  colour: red\n")


def test_missing_method_rule():
    """validate's rule for a missing method (clang._missing_allowed), shared by
    `via` and `optional`: only a real LibCarla may lack it, and for `optional`
    only a ref listed in missing_in; never the mock."""
    pytest.importorskip("clang.cindex")
    from tools.bindgen.clang import _missing_allowed

    s = spec.load()
    optional = next(f for f in s.functions if f.optional)
    assert _missing_allowed(optional, "libcarla", "0.10.0") is None
    assert _missing_allowed(optional, "libcarla", "ue5-dev") is not None
    assert _missing_allowed(optional, "mock", "unknown") is not None
    via = next((f for f in s.functions if f.via), None)
    if via is not None:
        assert _missing_allowed(via, "libcarla", "ue5-dev") is None
        assert _missing_allowed(via, "mock", "unknown") is not None
    plain = next(f for f in s.functions if not f.via and not f.optional)
    assert _missing_allowed(plain, "libcarla", "0.10.0") == "no such method"
