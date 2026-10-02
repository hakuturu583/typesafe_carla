"""Spec checks of the binding generator (tools/bindgen/spec.py): malformed
entries fail with SpecError instead of generating wrong code."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

from tools.bindgen import spec

ROOT = Path(__file__).resolve().parent.parent


def _load(tmp_path: Path, entry: str, types_extra: str = "",
          self_: str = "{type: tsc_thing_t, name: thing, get: thing_of}") -> spec.Spec:
    shutil.copy(ROOT / "bindings" / "types.yaml", tmp_path / "types.yaml")
    if types_extra:
        with open(tmp_path / "types.yaml", "a") as f:
            f.write(types_extra)
    (tmp_path / "thing.yaml").write_text(
        "class: carla::client::Thing\nprefix: thing\n"
        f"self: {self_}\n"
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
    assert g.body() == 'require_ptr(out, "out"); string_list_assign(out, g_compat(thing_of(thing)));'


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
    ("f: {args: {flag: bool}}", "missing key 'call'"),
    ("f: {new: \"yes\", out: map_handle}", "new must be true or false"),
    ("f: {new: true, call: F, out: map_handle}", "a constructor (new: true) has no call"),
    ("f: {new: true, args: {flag: bool}}", "a constructor needs a handle output"),
    ("f: {new: true, out: string}", "a constructor needs a handle output"),
])
def test_spec_errors(tmp_path, entry, message):
    with pytest.raises(spec.SpecError) as e:
        _load(tmp_path, entry)
    assert message in str(e.value)


def test_constructor(tmp_path):
    """`new: true` (issue #39): a constructor has no self parameter; it makes
    the class's object from the arguments and returns a new handle. `call` is
    the class's own name, which is how validate finds the constructors."""
    s = _load(tmp_path, "new: {new: true, args: {name: string_in, flag: bool}, out: map_handle}")
    (f,) = s.functions
    assert f.constructor and f.call == "Thing" and f.name == "tsc_thing_new"
    assert f.c_params() == ["const char *name, size_t name_len", "int32_t flag", "tsc_map_t **out"]
    assert f.codon_params() == ["cobj, int", "i32", "Ptr[cobj]"]
    assert f.body() == ("return new tsc_map(std::make_shared<carla::client::Thing>("
                        'to_string(name, name_len, "name"), flag != 0));')


def test_self_by_value(tmp_path):
    """`self.codon` (issue #31): a value type bound by pointer, e.g. a
    Transform, crosses as Ptr[<struct>] in Codon instead of an opaque handle."""
    s = _load(tmp_path, "m: {call: GetMatrix, out: {type: matrix4x4, name: out16}}",
              self_="{type: const tsc_transform_t, name: transform, get: transform_of, "
                    "codon: \"Ptr[CTransform]\"}")
    (m,) = s.functions
    assert m.c_params() == ["const tsc_transform_t *transform", "double *out16"]
    assert m.codon_params() == ["Ptr[CTransform]", "Ptr[float]"]
    assert m.body() == ('require_ptr(out16, "out16"); '
                        "copy_matrix(transform_of(transform).GetMatrix(), out16);")


def test_unknown_self_key(tmp_path):
    with pytest.raises(spec.SpecError, match=r"self: unknown keys \['colour'\]"):
        _load(tmp_path, "f: {call: F}", self_="{type: t, name: n, get: g, colour: red}")


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


def test_list_accessors():
    """bindings/lists.yaml: a size function and one getter per output type."""
    s = spec.load()
    by_name = {f.name: f for f in s.lists}
    size, get = by_name["tsc_world_snapshot_size"], by_name["tsc_world_snapshot_get"]
    assert size.ret == "size_t" and size.c_params() == ["const tsc_world_snapshot_t *snapshot"]
    assert get.c_params() == ["const tsc_world_snapshot_t *snapshot", "size_t index",
                              "tsc_actor_snapshot_t *out"]
    assert 'list_at(check_handle(snapshot, "snapshot", TSC_KIND_WORLD_SNAPSHOT)->actors' in get.body()
    assert {"tsc_landmark_list_get", "tsc_landmark_list_get_landmark"} <= set(by_name)
    assert not set(by_name) & {f.name for f in s.functions}  # validate skips them


def test_outputs_are_checked_before_the_call(tmp_path):
    """C++17 evaluates the right side of `=` first, so `*require_ptr(out) =
    call` would run the call (and its side effects) before rejecting a NULL
    output. Plain outputs go through assign_out (check, then call, zero on
    failure); `assign` outputs check their `require` parameters first."""
    s = _load(tmp_path, """
plain: {call: P, args: {t: finite}, out: {type: uint64, name: out_frame}}
text: {call: T, out: string}
buffer: {call: B, out: transform_buffer}
maybe: {call: M, out: nullable_bool}
""")
    plain, text, buffer, maybe = s.functions
    assert plain.body() == ('assign_out(out_frame, "out_frame", [&] { return '
                            'thing_of(thing).P(check_finite(t, "t")); });')
    assert text.body() == 'require_ptr(out, "out"); string_assign(out, thing_of(thing).T());'
    assert buffer.body().startswith('require_ptr(out_count, "out_count"); copy_out(')
    assert maybe.body() == "store_if(out, thing_of(thing).M() ? 1 : 0);"


def test_handle_type_defaults(tmp_path):
    """A handle type's codon defaults to cobj; as an output (no to_carla) its
    from_carla defaults to a new handle of the result."""
    s = _load(tmp_path, "f: {call: F, out: {type: gadget_handle, name: out_gadget}}",
              "gadget_handle: {c: tsc_gadget_t, handle: true}\n"
              "gadget_in: {c: tsc_gadget_t, handle: true, to_carla: 'gadget_of({})'}\n")
    out, inp = s.types["gadget_handle"], s.types["gadget_in"]
    assert out.codon == inp.codon == "cobj"
    assert out.from_carla == "new tsc_gadget({})" and inp.from_carla == "{}"
    (f,) = s.functions
    assert f.c_params()[-1] == "tsc_gadget_t **out_gadget"
    assert f.body() == "return new tsc_gadget(thing_of(thing).F());"


def test_new_handle_names_the_output():
    """emit.shim passes a non-default output name to new_handle, so its NULL
    check names the C parameter."""
    from tools.bindgen import emit

    text = emit.shim(spec.load())
    assert 'return new_handle(__func__, out_world, [&] {' in text
    assert '}, "out_world");' in text
    assert '}, "out");' not in text  # the default name is not passed


@pytest.mark.parametrize("lists, message", [
    ("l: {what: thing list, get: actor_handle}", "missing keys ['items']"),
    ("l: {items: '{}->x', get: actor_handle}", "missing keys ['what']"),
    ("l: {items: '{}->x', what: w, get: {type: actor_handle, colour: red}}", "unknown keys"),
    ("l: {items: '{}->x', what: w, get: string_in}", "is input-only"),
    ("thing: {items: '{}->x', what: w, get: actor_handle}\n", "tsc_thing_get is also defined"),
])
def test_list_spec_errors(tmp_path, lists, message):
    (tmp_path / "lists.yaml").write_text(lists + "\n")
    with pytest.raises(spec.SpecError) as e:
        _load(tmp_path, "get: {call: G, out: bool}")
    assert message in str(e.value)
