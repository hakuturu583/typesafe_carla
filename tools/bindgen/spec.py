"""Loads and checks the binding specification in bindings/."""

from __future__ import annotations

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
    # C parameter(s) for an input, if not `c name`; `{name}` is the argument
    # name, e.g. "const char *{name}, size_t {name}_len" (codon and invalid
    # then list one value per C parameter).
    c_param_template: str | None = None
    cpp: tuple[str, ...] = ()
    invalid: str = "0"  # a C value the conversion rejects, for test_generated

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
    self_type: str
    self_name: str
    self_get: str
    args: tuple[Arg, ...]
    out: Out | None
    doc: str | None

    def c_params(self) -> list[str]:
        params = [f"{self.self_type} *{self.self_name}"]
        params += [a.type.c_param(a.name) for a in self.args]
        if self.out:
            stars = "**" if self.out.type.handle else "*"
            params.append(f"{self.out.type.c} {stars}{self.out.name}")
        return params

    def codon_params(self) -> list[str]:
        params = ["cobj"] + [a.type.codon_param() for a in self.args]
        if self.out:
            params.append(f"Ptr[{self.out.type.codon}]")
        return params

    def body(self) -> str:
        call = (f"{self.self_get}({self.self_name}).{self.call}"
                f"({', '.join(a.to_carla() for a in self.args)})")
        if self.out is None:
            return f"{call};"
        t = self.out.type
        if t.handle:  # the statement of new_handle's lambda (see emit.shim)
            return f"return {t.from_carla.replace('{}', call)};"
        if t.assign:
            return t.assign.replace("{out}", self.out.name).replace("{}", call) + ";"
        return f'*require_ptr({self.out.name}, "{self.out.name}") = {t.from_carla.replace("{}", call)};'


@dataclass(frozen=True)
class Spec:
    types: dict[str, Type]
    functions: tuple[Function, ...]

    def _group(self, key: str) -> dict[str, list[Function]]:
        out: dict[str, list[Function]] = {}
        for f in self.functions:
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
                             "invalid", "c_param"}
        if unknown:
            raise SpecError(f"{path.name}: {name}: unknown keys {sorted(unknown)}")
        types[name] = Type(name=name, c=t["c"], codon=t["codon"], to_carla=t.get("to_carla", "{}"),
                           from_carla=t.get("from_carla", "{}"), assign=t.get("assign"),
                           struct=t.get("struct", False), handle=t.get("handle", False),
                           cpp=tuple(t.get("cpp", ())), invalid=str(t.get("invalid", "0")),
                           c_param_template=t.get("c_param"))
    return types


def load(bindings: Path = BINDINGS) -> Spec:
    types = _load_types(bindings / "types.yaml")

    def type_of(name: str, where: str) -> Type:
        if name not in types:
            raise SpecError(f"{where}: unknown type {name!r} (see bindings/types.yaml)")
        return types[name]

    functions: list[Function] = []
    seen: dict[str, str] = {}
    for path in sorted(bindings.glob("*.yaml")):
        if path.name == "types.yaml":
            continue
        raw = _load_yaml(path)
        unknown = set(raw) - {"class", "prefix", "self", "blocks"}
        if unknown:
            raise SpecError(f"{path.name}: unknown keys {sorted(unknown)}")
        self_ = raw["self"]
        for block, entries in raw["blocks"].items():
            for short, entry in entries.items():
                where = f"{path.name}: {short}"
                unknown = set(entry) - {"call", "args", "out", "doc"}
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
                name = f"tsc_{raw['prefix']}_{short}"
                if name in seen:
                    raise SpecError(f"{where}: {name} is also defined in {seen[name]}")
                seen[name] = path.name
                functions.append(Function(
                    name=name, block=block, spec_file=path.name, cpp_class=raw["class"],
                    call=entry["call"], self_type=self_["type"], self_name=self_["name"],
                    self_get=self_["get"], args=args, out=out, doc=entry.get("doc")))
    blocks: dict[str, str] = {}
    for f in functions:
        if blocks.setdefault(f.block, f.spec_file) != f.spec_file:
            raise SpecError(f"block {f.block!r} is defined in {blocks[f.block]} and {f.spec_file}")
    return Spec(types=types, functions=tuple(functions))
