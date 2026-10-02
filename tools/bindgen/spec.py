"""Loads and checks the binding specification in bindings/."""

from __future__ import annotations

import re

from dataclasses import dataclass
from pathlib import Path

import yaml


class _UniqueKeyLoader(yaml.SafeLoader):
    """Rejects duplicate mapping keys, which PyYAML otherwise silently collapses
    (a repeated function name would replace the earlier binding)."""

    def construct_mapping(self, node, deep=False):
        keys = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=deep)
            if key in keys:
                raise SpecError(f"{node.start_mark.name}:{key_node.start_mark.line + 1}: "
                                f"duplicate key {key!r}")
            keys.add(key)
        return super().construct_mapping(node, deep)


def _load_yaml(path: Path):
    with path.open() as stream:  # a stream, so errors name the file
        return yaml.load(stream, Loader=_UniqueKeyLoader)

ROOT = Path(__file__).resolve().parents[2]
BINDINGS = ROOT / "bindings"


class SpecError(Exception):
    pass


@dataclass(frozen=True)
class Type:
    name: str
    c: str
    codon: str
    to_carla: str = "{}"
    from_carla: str = "{}"
    assign: str | None = None  # output via a function instead of `*out = ...`
    struct: bool = False  # crosses the ABI by pointer
    # An opaque handle pointer. As an output, from_carla creates the handle
    # (or yields NULL) and the C function returns it through `T **out`.
    handle: bool = False
    # C parameter(s), if not the default for an input or output; `{name}` is
    # the argument name, e.g. "const char *{name}, size_t {name}_len" (codon
    # and invalid then list one value per C parameter).
    c_param_template: str | None = None
    cpp: tuple[str, ...] = ()
    invalid: str = "0"  # a C value the conversion rejects, for test_generated
    # For an `assign` output: the C parameters (`{out}` is the output name)
    # checked non-NULL before the LibCarla call, so a NULL output fails
    # before anything happens. Default: `{out}`, or none with c_param.
    require: tuple[str, ...] = ()

    def c_param(self, name: str) -> str:
        if self.c_param_template:
            return self.c_param_template.replace("{name}", name)
        if self.struct:
            return f"const {self.c} *{name}"
        if self.handle:
            return f"{self.c} *{name}"
        return f"{self.c} {name}"

    def codon_param(self) -> str:
        return f"Ptr[{self.codon}]" if self.struct else self.codon


@dataclass(frozen=True)
class Arg:
    name: str
    type: Type

    def to_carla(self) -> str:
        value = f'*require_ptr({self.name}, "{self.name}")' if self.type.struct else self.name
        return self.type.to_carla.replace("{name}", self.name).replace("{}", value)


@dataclass(frozen=True)
class Out:
    name: str
    type: Type


@dataclass(frozen=True)
class Function:
    name: str  # the C function
    block: str
    spec_file: str
    cpp_class: str
    call: str  # the LibCarla method
    via: str | None  # a tsc:: helper called as via(self, args...) instead (version differences)
    self_type: str
    self_name: str
    self_get: str
    args: tuple[Arg, ...]
    out: Out | None
    doc: str | None
    # Set when a supported LibCarla lacks the method: the feature's name for
    # the TSC_ERROR that the call raises there (TSC_CALL_OPTIONAL), and the
    # CARLA refs (the build's TSC_CARLA_GIT_REF) whose LibCarla lacks it;
    # `validate` accepts the method missing only on those.
    optional: str | None = None
    missing_in: tuple[str, ...] = ()
    # The Codon FFI type of the self parameter: an opaque handle by default,
    # Ptr[<struct>] for a value type bound by pointer (e.g. Transform).
    self_codon: str = "cobj"
    # A list accessor (bindings/lists.yaml) calls no LibCarla method: `expr` is
    # the whole C++ expression, and `ret` the C result (size_t for tsc_*_size).
    expr: str | None = None
    ret: str = "tsc_status_t"

    def c_params(self) -> list[str]:
        params = [f"{self.self_type} *{self.self_name}"]
        params += [a.type.c_param(a.name) for a in self.args]
        if self.out:
            t = self.out.type
            stars = "**" if t.handle else "*"
            params.append(t.c_param(self.out.name) if t.c_param_template
                          else f"{t.c} {stars}{self.out.name}")
        return params

    def codon_params(self) -> list[str]:
        params = [self.self_codon] + [a.type.codon_param() for a in self.args]
        if self.out:
            t = self.out.type
            params.append(t.codon if t.c_param_template else f"Ptr[{t.codon}]")
        return params

    def body(self) -> str:
        if self.ret != "tsc_status_t":
            return self.expr
        self_ = f"{self.self_get}({self.self_name})"
        args = [a.to_carla() for a in self.args]
        if self.optional:
            return f'TSC_CALL_OPTIONAL({", ".join([self_, self.call, chr(34) + self.optional + chr(34)] + args)});'
        if self.expr:
            call = self.expr
        elif self.via and args:
            # The handle is checked before the arguments are converted, as in a
            # member call (function arguments are evaluated in no fixed order).
            call = f"[&](auto &self_) {{ return {self.via}({', '.join(['self_'] + args)}); }}({self_})"
        elif self.via:
            call = f"{self.via}({self_})"
        else:
            call = f"{self_}.{self.call}({', '.join(args)})"
        if self.out is None:
            return f"{call};"
        t = self.out.type
        if t.handle:  # the statement of new_handle's lambda (see emit.shim)
            return f"return {t.from_carla.replace('{}', call)};"
        # The output is checked before the call (C++17 evaluates the right
        # side of `=` first): a NULL output must fail before any side effect.
        if t.assign:
            checks = [f'require_ptr({r}, "{r}"); ' for r in
                      (r.replace("{out}", self.out.name) for r in t.require)]
            return "".join(checks) + t.assign.replace("{out}", self.out.name).replace("{}", call) + ";"
        # assign_out: checks the output, and zeroes it if the call fails.
        name = self.out.name
        return f'assign_out({name}, "{name}", [&] {{ return {t.from_carla.replace("{}", call)}; }});'


@dataclass(frozen=True)
class Spec:
    types: dict[str, Type]
    functions: tuple[Function, ...]  # each calls one LibCarla method
    lists: tuple[Function, ...] = ()  # list accessors (bindings/lists.yaml)

    def generated(self) -> tuple[Function, ...]:
        return self.functions + self.lists

    def _group(self, key: str) -> dict[str, list[Function]]:
        out: dict[str, list[Function]] = {}
        for f in self.functions if key == "cpp_class" else self.generated():
            out.setdefault(getattr(f, key), []).append(f)
        return out

    def blocks(self) -> dict[str, list[Function]]:
        return self._group("block")

    def classes(self) -> dict[str, list[Function]]:
        return self._group("cpp_class")


def _load_types(path: Path) -> dict[str, Type]:
    raw = _load_yaml(path)
    types = {}
    for name, t in raw.items():
        unknown = set(t) - {"c", "codon", "to_carla", "from_carla", "assign", "struct", "handle", "cpp",
                             "invalid", "c_param", "require"}
        if unknown:
            raise SpecError(f"{path.name}: {name}: unknown keys {sorted(unknown)}")
        handle = t.get("handle", False)
        # A handle output defaults to a new handle of the result: tsc_x_t -> new tsc_x({}).
        new = f"new {t['c'].removeprefix('const ').removesuffix('_t')}({{}})"
        types[name] = Type(name=name, c=t["c"], codon=t.get("codon", "cobj") if handle else t["codon"],
                           to_carla=t.get("to_carla", "{}"),
                           from_carla=t.get("from_carla", new if handle and "to_carla" not in t else "{}"),
                           assign=t.get("assign"),
                           struct=t.get("struct", False), handle=handle,
                           cpp=tuple(t.get("cpp", ())), invalid=str(t.get("invalid", "0")),
                           c_param_template=t.get("c_param"),
                           require=tuple(t.get("require", [] if "c_param" in t else ["{out}"])))
    return types


def _load_lists(path: Path, type_of) -> list[Function]:
    """bindings/lists.yaml: tsc_<list>_size and one element getter per output type."""
    functions = []
    for name, entry in (_load_yaml(path) if path.exists() else {}).items():
        where = f"{path.name}: {name}"
        if not isinstance(entry, dict):
            raise SpecError(f"{where}: expected a mapping")
        missing = {"items", "what"} - set(entry)
        if missing:
            raise SpecError(f"{where}: missing keys {sorted(missing)}")
        self_, items = entry.get("self", "list"), entry["items"]
        kind, handle = f"TSC_KIND_{name.upper()}", f"const tsc_{name}_t"
        common = dict(block=f"{name}_items", spec_file=path.name, cpp_class="", call="", via=None,
                      self_type=handle, self_name=self_, self_get="", doc=None)
        functions.append(Function(
            name=f"tsc_{name}_size", args=(), out=None, ret="size_t", **common,
            expr=f"if ({self_} == nullptr || {self_}->kind != {kind}) return 0;\n"
                 f"return {items.format(self_)}.size();"))
        checked = items.format(f'check_handle({self_}, "{self_}", {kind})')
        for getter, o in entry.items():
            if getter in ("self", "items", "what"):
                continue
            o = {"type": o} if isinstance(o, str) else o
            if not isinstance(o, dict) or "type" not in o or set(o) - {"type", "name"}:
                raise SpecError(f"{where}: {getter}: unknown keys, or not an output type "
                                "(a type name, or {type, name})")
            out = Out(o.get("name", "out"), type_of(o["type"], f"{where}: {getter}"))
            if out.type.c_param_template and not out.type.assign:
                raise SpecError(f"{where}: {getter}: {out.type.name} is input-only")
            functions.append(Function(
                name=f"tsc_{name}_{getter}", args=(Arg("index", type_of("index", path.name)),),
                out=out, expr=f'list_at({checked}, index, "{entry["what"]}")', **common))
    return functions


def load(bindings: Path = BINDINGS) -> Spec:
    types = _load_types(bindings / "types.yaml")

    def type_of(name: str, where: str) -> Type:
        if name not in types:
            raise SpecError(f"{where}: unknown type {name!r} (see bindings/types.yaml)")
        return types[name]

    functions: list[Function] = []
    seen: dict[str, str] = {}
    for path in sorted(bindings.glob("*.yaml")):
        if path.name in ("types.yaml", "lists.yaml"):
            continue
        raw = _load_yaml(path)
        unknown = set(raw) - {"class", "prefix", "self", "blocks"}
        if unknown:
            raise SpecError(f"{path.name}: unknown keys {sorted(unknown)}")
        self_ = raw["self"]
        unknown = set(self_) - {"type", "name", "get", "codon"}
        if unknown:
            raise SpecError(f"{path.name}: self: unknown keys {sorted(unknown)}")
        for block, entries in raw["blocks"].items():
            for short, entry in entries.items():
                where = f"{path.name}: {short}"
                unknown = set(entry) - {"call", "via", "args", "out", "doc", "optional"}
                if unknown:
                    raise SpecError(f"{where}: unknown keys {sorted(unknown)}")
                args = tuple(Arg(n, type_of(t, where)) for n, t in (entry.get("args") or {}).items())
                for a in args:
                    if a.type.assign:
                        raise SpecError(f"{where}: {a.type.name} is output-only")
                out = None
                if "out" in entry:
                    o = entry["out"]
                    o = {"type": o} if isinstance(o, str) else o
                    out = Out(o.get("name", "out"), type_of(o["type"], where))
                    if out.type.handle and out.type.from_carla == "{}":
                        raise SpecError(f"{where}: {out.type.name} has no from_carla to create "
                                        "the output handle")
                    if out.type.c_param_template and not out.type.assign:
                        raise SpecError(f"{where}: {out.type.name} is input-only")
                optional, missing_in = None, ()
                if "optional" in entry:
                    if out:
                        raise SpecError(f"{where}: an optional method has no output")
                    if "via" in entry:
                        raise SpecError(f"{where}: `optional` and `via` exclude each other")
                    o = entry["optional"]
                    if not isinstance(o, dict) or set(o) != {"name", "missing_in"} or \
                            not isinstance(o["missing_in"], list) or not o["missing_in"]:
                        raise SpecError(f"{where}: optional must be {{name: ..., missing_in: [refs]}}")
                    optional, missing_in = str(o["name"]), tuple(str(r) for r in o["missing_in"])
                c_params = [self_["name"]] + [a.type.c_param(a.name) for a in args]
                if out:
                    c_params.append(out.type.c_param(out.name) if out.type.c_param_template
                                    else out.name)
                params = [n for c in c_params for n in re.findall(r"(\w+)\s*(?:,|$)", c)]
                duplicate = sorted({n for n in params if params.count(n) > 1})
                if duplicate:
                    raise SpecError(f"{where}: duplicate C parameter(s) {duplicate}")
                name = f"tsc_{raw['prefix']}_{short}"
                if name in seen:
                    raise SpecError(f"{where}: {name} is also defined in {seen[name]}")
                seen[name] = path.name
                functions.append(Function(
                    name=name, block=block, spec_file=path.name, cpp_class=raw["class"],
                    call=entry["call"], via=entry.get("via"), self_type=self_["type"], self_name=self_["name"],
                    self_get=self_["get"], args=args, out=out, doc=entry.get("doc"),
                    optional=optional, missing_in=missing_in,
                    self_codon=self_.get("codon", "cobj")))
    lists = _load_lists(bindings / "lists.yaml", type_of)
    for f in lists:
        if f.name in seen:
            raise SpecError(f"lists.yaml: {f.name} is also defined in {seen[f.name]}")
        seen[f.name] = f.spec_file
    blocks: dict[str, str] = {}
    for f in functions + lists:
        if blocks.setdefault(f.block, f.spec_file) != f.spec_file:
            raise SpecError(f"block {f.block!r} is defined in {blocks[f.block]} and {f.spec_file}")
    return Spec(types=types, functions=tuple(functions), lists=tuple(lists))
