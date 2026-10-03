"""Builds `carla`: typesafe_carla as a CPython package (`codon build --pyext`).

CPython can then run CARLA's own Python tests unmodified, with `import carla`
resolving to typesafe_carla. This is a test and compatibility artifact. The
typesafe_carla core stays free of CPython; this tool only generates code
around the unchanged package.

Codon 0.19's extension export cannot take typesafe_carla's classes as they
are. It exports only the compiled module's own classes and functions,
private ones included. It crashes on static parameters (`T: type`,
`Literal`). It exports an untyped parameter as `pyobj`. And it refuses
polymorphic classes, which every class in the Vector3D, Actor and
SensorData hierarchies is. So the build generates two layers:

* `_carla.codon`, compiled with `--pyext` into `carla/_carla.so`. It imports
  typesafe_carla unchanged and defines:
  - one opaque box class `B_<Class>` per public class (one field, so it is
    not polymorphic);
  - `__to_py__` / `__from_py__` for the library's classes, which box and
    unbox them (an actor reaches Python as its concrete class);
  - one exported function per method, property, field and constructor,
    `<Class>__<member>`, with the library's own parameter types and
    defaults, so the exporter binds arguments and picks overloads.
* `carla/__init__.py` (compat/pycarla/_runtime.py and the generated
  `_spec.json`): Python classes mirroring the CARLA Python API. Each holds a
  box and forwards to those functions. Being Python classes, they give real
  inheritance (`isinstance(v, carla.Actor)`), the enumerations, and
  `carla.command`.

The generation is mechanical, driven by the Codon sources (parsed with
Python's ast). A member it cannot wrap becomes a stub that raises
NotImplementedError("pycarla: <Class>.<member> not wrapped: <reason>");
`python -m tools.pycarla --report` lists them.

    python -m tools.pycarla [-o DIR]          # default DIR: <build dir>/pycarla
    PYTHONPATH=DIR python -c 'import carla'
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "codon" / "typesafe_carla"
RUNTIME = ROOT / "compat" / "pycarla" / "_runtime.py"
PRUNED = Path(os.environ.get("PYCARLA_PRUNED") or ROOT / "compat" / "pycarla" / "pruned.json")
MODULE = "_carla"
PACKAGE_NAME = "carla"

PRIMS = {"int", "float", "bool", "str", "NoneType", "None"}
CONTAINERS = {"List", "Optional", "Tuple", "Dict", "Set"}
# Modules whose @extend blocks are not wrapped: Actor's Python-API shortcuts
# to subclass methods. Here every actor already has its concrete class.
SKIP_EXTEND_MODULES = {"_actor_compat"}
# typesafe_carla's exceptions; CARLA's Python API raises RuntimeError.
EXCEPTIONS = ["CarlaError", "TimeoutError", "ActorTypeError", "VersionError"]
# Members never exported (Codon protocol methods, or done another way).
SKIP_MEMBERS = {"__init__", "__to_py__", "__from_py__", "__del__", "__copy__", "__deepcopy__",
                "__pickle__", "__unpickle__", "__raw__", "__iter__", "__hash__"}

# An untyped (generic) parameter has no type to export it with. The types it
# accepts, by parameter name, from the library's documentation of them; each
# becomes an overload. Members with other untyped parameters are stubs.
ACTOR_CLASSES = ["Vehicle", "Walker", "WalkerAIController", "Sensor", "TrafficLight",
                 "TrafficSign", "Actor"]
VECTOR_CLASSES = ["Location", "Vector3D"]
GENERIC_PARAMS = {
    **{n: VECTOR_CLASSES for n in (
        "location", "vector", "velocity", "angular_velocity", "impulse", "force",
        "angular_impulse", "torque", "in_point", "world_point", "destination", "begin", "end",
        "initial_location", "final_location", "direction", "point", "v")},
    "attach_to": ACTOR_CLASSES + ["NoneType"],
    "actor": ACTOR_CLASSES,
    "other_actor": ACTOR_CLASSES,
    "callback": ["pyobj"],
    "other": ["@self", "@vectors"],   # operators: the same class (vectors: Location / Vector3D too)
    **{n: VECTOR_CLASSES for n in (
        "offset", "suspension_axis", "suspension_force_offset", "old_location", "center_of_mass",
        "inertia_tensor_scale", "extent")},
    "projection": ["GeoProjectionTM", "GeoProjectionUTM", "GeoProjectionWebMerc", "GeoProjectionLCC2SP"],
    "path": ["List[Location]", "List[Vector3D]"],
}
GENERIC_PARAMS["in_point"] = VECTOR_CLASSES + ["List[Location]", "List[Vector3D]"]
# With more than this many defaulted generic parameters, each takes one
# Optional of its most general type instead of one overload per type (the
# product would explode, e.g. WheelPhysicsControl's six vectors).
MAX_EXPANDED = 2


# ---------------------------------------------------------------------------
# Inventory of the Codon sources
# ---------------------------------------------------------------------------

@dataclass
class Param:
    name: str
    ann: str | None
    default: str | None


@dataclass
class Func:
    name: str
    module: str
    params: list[Param]          # without self
    ret: str | None
    kind: str = "method"         # method, property, setter, static
    varargs: bool = False
    self_typed: bool = False     # `self: S, ..., S: type`


@dataclass
class Cls:
    name: str
    module: str
    is_tuple: bool
    bases: list[str]
    fields: list[tuple[str, str]] = field(default_factory=list)
    members: dict[str, list[Func]] = field(default_factory=dict)
    classvars: dict[str, str] = field(default_factory=dict)


def _source(name: str) -> str:
    """A module's source, made parseable by Python's ast."""
    lines = []
    for line in (PACKAGE / f"{name}.codon").read_text().splitlines():
        if line.startswith("from C import"):
            line = ""
        lines.append(re.sub(r"^(class \w+)\[[^\]]*\]", r"\1", line))
    return "\n".join(lines)


def _unparse(node) -> str | None:
    return None if node is None else ast.unparse(node)


def _strip_static(base: str) -> str:
    return re.sub(r"^Static\[(.*)\]$", r"\1", base)


def _func(node: ast.FunctionDef, module: str) -> Func:
    args = node.args
    pos = args.posonlyargs + args.args
    defaults = [None] * (len(pos) - len(args.defaults)) + list(args.defaults)
    params = [Param(a.arg, _unparse(a.annotation), _unparse(d)) for a, d in zip(pos, defaults)]
    params += [Param(a.arg, _unparse(a.annotation), _unparse(d))
               for a, d in zip(args.kwonlyargs, args.kw_defaults)]
    self_typed = bool(params) and params[0].name == "self" and params[0].ann is not None
    if params and params[0].name == "self":
        params = params[1:]
    kind = "method"
    for d in node.decorator_list:
        text = ast.unparse(d)
        if text == "property":
            kind = "property"
        elif text.endswith(".setter"):
            kind = "setter"
        elif text == "staticmethod":
            kind = "static"
    return Func(node.name, module, params, _unparse(node.returns), kind,
                bool(args.vararg or args.kwarg), self_typed)


class Inventory:
    """typesafe_carla's public API, from its Codon sources."""

    def __init__(self) -> None:
        self.classes: dict[str, Cls] = {}
        self.module_names: dict[str, set[str]] = {}
        self.functions: dict[str, list[Func]] = {}
        self.aliases: dict[str, str] = {}
        self.command: list[str] = []
        self.constants: dict[str, dict] = {}   # module -> its literal top-level constants
        self.unions: dict[str, list[str]] = {}  # `X = Union[A, B]` aliases
        trees = {p.stem: ast.parse(_source(p.stem)) for p in sorted(PACKAGE.glob("*.codon"))}
        extends = []
        for m, tree in trees.items():
            names: set[str] = set()
            for node in tree.body:
                if isinstance(node, ast.ClassDef):
                    names.add(node.name)
                    decos = [ast.unparse(d) for d in node.decorator_list]
                    if "extend" in decos:
                        extends.append((m, node))
                    else:
                        self.classes[node.name] = self._class(node, m, decos)
                elif isinstance(node, ast.FunctionDef):
                    names.add(node.name)
                    if not node.name.startswith("_"):
                        self.functions.setdefault(node.name, []).append(_func(node, m))
                elif isinstance(node, ast.Assign) and len(node.targets) == 1 \
                        and isinstance(node.targets[0], ast.Name):
                    names.add(node.targets[0].id)
                    if isinstance(node.value, ast.Name):
                        self.aliases[node.targets[0].id] = node.value.id
                    elif isinstance(node.value, ast.Subscript) and ast.unparse(node.value.value) == "Union":
                        elts = node.value.slice.elts if isinstance(node.value.slice, ast.Tuple) else [node.value.slice]
                        self.unions[node.targets[0].id] = [ast.unparse(e) for e in elts]
                elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
                    names.add(node.target.id)
                    if node.value is not None and not node.target.id.startswith("_"):
                        try:
                            self.constants.setdefault(m, {})[node.target.id] = ast.literal_eval(node.value)
                        except (ValueError, SyntaxError):
                            pass
                elif isinstance(node, (ast.Import, ast.ImportFrom)):
                    names.update(a.asname or a.name.split(".")[0] for a in node.names)
            self.module_names[m] = names
        self.command = [n.name for n in trees["command"].body
                        if isinstance(n, ast.FunctionDef) and not n.name.startswith("_")]
        for m, node in extends:
            if m not in SKIP_EXTEND_MODULES and node.name in self.classes:
                self._members(self.classes[node.name], node, m)
        self.int_enums = self._int_enums(trees)
        self.public = [a.asname or a.name for node in trees["__init__"].body
                       if isinstance(node, ast.ImportFrom) and node.level == 1 and node.module != "_ffi"
                       for a in node.names]

    def _class(self, node: ast.ClassDef, module: str, decos: list[str]) -> Cls:
        c = Cls(node.name, module, "tuple" in decos, [ast.unparse(b) for b in node.bases])
        for stmt in node.body:
            if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name):
                ann = ast.unparse(stmt.annotation)
                if ann.startswith("ClassVar"):
                    c.classvars[stmt.target.id] = _unparse(stmt.value) or ""
                else:
                    c.fields.append((stmt.target.id, ann))
        self._members(c, node, module)
        return c

    @staticmethod
    def _members(c: Cls, node: ast.ClassDef, module: str) -> None:
        for stmt in node.body:
            if isinstance(stmt, ast.FunctionDef):
                f = _func(stmt, module)
                key = stmt.name + (".setter" if f.kind == "setter" else "")
                c.members.setdefault(key, []).append(f)

    @staticmethod
    def _int_enums(trees) -> dict[str, dict[str, int]]:
        """Integer enumerations (`Name = _Name()`, _Name an _IntEnum): members."""
        members: dict[str, dict[str, int]] = {}
        for tree in trees.values():
            for node in tree.body:
                if isinstance(node, ast.ClassDef) and any(ast.unparse(b) == "_IntEnum" for b in node.bases):
                    vals: dict[str, int] = {}
                    for stmt in node.body:
                        if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name) \
                                and ast.unparse(stmt.annotation) == "int" and stmt.value is not None:
                            vals[stmt.target.id] = eval(compile(ast.Expression(stmt.value), "", "eval"),
                                                        {"__builtins__": {}}, dict(vals))
                    members[node.name] = vals
        out = {}
        for tree in trees.values():
            for node in tree.body:
                if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) \
                        and isinstance(node.value.func, ast.Name) and node.value.func.id in members:
                    out[node.targets[0].id] = members[node.value.func.id]
        return out

    def resolve(self, name: str) -> str:
        seen = set()
        while name in self.aliases and name not in seen:
            seen.add(name)
            name = self.aliases[name]
        return name

    def public_classes(self) -> list[str]:
        out = []
        for n in self.public:
            r = self.resolve(n)
            if r in self.classes and r not in out and r not in EXCEPTIONS:
                out.append(r)
        return out

    def is_typed_enum(self, name: str) -> bool:
        c = self.classes.get(name)
        return bool(c and c.is_tuple and c.fields == [("value", "int")]
                    and any(v.startswith(f"{name}(") for v in c.classvars.values()))

    def mro(self, name: str) -> list[str]:
        out = [name]
        for b in self.classes[name].bases:
            b = _strip_static(b)
            if b in self.classes:
                out += [x for x in self.mro(b) if x not in out]
        return out

    def all_members(self, name: str) -> dict[str, list[Func]]:
        """Own and inherited members; the nearest class's definition wins."""
        out: dict[str, list[Func]] = {}
        for c in self.mro(name):
            for k, fs in self.classes[c].members.items():
                out.setdefault(k, fs)
        return out

    def all_fields(self, name: str) -> list[tuple[str, str, str]]:
        """(name, annotation, module) of own and inherited fields."""
        out: list[tuple[str, str, str]] = []
        for c in reversed(self.mro(name)):
            cls = self.classes[c]
            out += [(n, a, cls.module) for n, a in cls.fields if n not in [x[0] for x in out]]
        return out


# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------

class Unsupported(Exception):
    pass


REALIZE = "import sys as _sys\ndef _realize_library_calls():\n"


def _split_params(sig: str) -> list[str]:
    """`a: List[int], b: Dict[str, int] = x` -> its parameters."""
    out, depth, cur = [], 0, ""
    for ch in sig:
        depth += ch in "[(" and 1 or (ch in "])" and -1 or 0)
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    return out + ([cur.strip()] if cur.strip() else [])


def guarded(stmt: str) -> str:
    """stmt. typesafe_carla's exceptions reach Python as BaseException (they
    have no CPython type); compat/pycarla/_runtime.py raises them as
    RuntimeError, as CARLA's Python API does."""
    return stmt


class Gen:
    def __init__(self, inv: Inventory, pruned: dict[str, str] | None = None) -> None:
        self.inv = inv
        self.public = inv.public_classes()
        only = os.environ.get("PYCARLA_ONLY")  # debugging: a subset of the classes
        if only:
            self.public = [c for c in self.public if c in only.split(",")]
        self.out: list[str] = []
        self.keys: list[str | None] = []  # per self.out chunk: its prune key
        self.pruned = pruned or {}        # prune key -> compiler error
        self.lines: list[tuple[int, int, str]] = []
        self.stubs: dict[str, str] = {}   # "Class.member" -> why it is not wrapped
        self.api: dict[str, dict] = {}    # what carla/__init__.py builds
        self.cls = ""                     # the class being generated
        self.realize_keys: list[str] = []
        self.variant_names: dict[str, list[str]] = {}  # function -> its overloads' names

    # -- types and defaults ----------------------------------------------------

    def ty(self, ann: str | None) -> str:
        """A library annotation as a type of the generated module."""
        if ann is None:
            raise Unsupported("untyped parameter")
        return self._ty(ast.parse(ann, mode="eval").body)

    def _ty(self, node) -> str:
        if isinstance(node, ast.Name):
            n = self.inv.resolve(node.id)
            if n in PRIMS:
                return "NoneType" if n == "None" else n
            if n in self.public:
                return f"_L_{n}"
            raise Unsupported(f"type {node.id}")
        if isinstance(node, ast.Constant) and node.value is None:
            return "NoneType"
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name) \
                and node.value.id in CONTAINERS:
            items = node.slice.elts if isinstance(node.slice, ast.Tuple) else [node.slice]
            return f"{node.value.id}[{', '.join(self._ty(i) for i in items)}]"
        raise Unsupported(f"type {ast.unparse(node)}")

    def default(self, expr: str | None, module: str) -> str | None:
        """A library default, its names qualified with their module."""
        if expr is None:
            return None
        public, names = self.public, self.inv.module_names.get(module, set())

        class Qualify(ast.NodeTransformer):
            def visit_Name(self, n):
                if n.id in public:
                    return ast.copy_location(ast.Name(f"_L_{n.id}", ast.Load()), n)
                if n.id in names:
                    return ast.copy_location(
                        ast.Attribute(ast.Name(f"_M_{module}", ast.Load()), n.id, ast.Load()), n)
                return n

        return ast.unparse(Qualify().visit(ast.parse(expr, mode="eval")))

    def variants(self, f: Func) -> list[list[tuple[str, str, str | None]]]:
        """Typed (name, type, default) parameter lists for one library overload."""
        if f.varargs:
            raise Unsupported("*args/**kwargs")
        if f.self_typed:
            raise Unsupported("self-typed generic method (Codon exporter)")
        lists: list[list[tuple[str, str, str | None]]] = [[]]
        n_defaulted = sum(1 for p in f.params if p.ann is None and p.default == "None")
        for p in f.params:
            if p.ann is not None and re.search(r"\b(type|Literal|Static)\b", p.ann):
                raise Unsupported(f"static parameter '{p.name}' (Codon exporter)")
            d = self.default(p.default, f.module)
            if p.ann is not None:
                t = self.ty(p.ann)
                lists = [lst + [(p.name, t, d)] for lst in lists]
                continue
            cands = GENERIC_PARAMS.get(p.name)
            if not cands:
                raise Unsupported(f"untyped parameter '{p.name}'")
            vector_family = self.cls in self.inv.classes and "Vector3D" in self.inv.mro(self.cls)
            cands = [x for c in cands for x in (
                [self.cls] if c == "@self" else
                ([v for v in VECTOR_CLASSES if v != self.cls] if vector_family else []) if c == "@vectors"
                else [c])]
            if d == "None" and n_defaulted > MAX_EXPANDED and cands[-1] != "pyobj":
                general = "Vector3D" if "Vector3D" in cands else cands[-1]
                lists = [lst + [(p.name, f"Optional[{self._cand(general)}]", "None")] for lst in lists]
                continue
            if d == "None" and "NoneType" not in cands:
                # Left out: passed as None (the library's default), not by
                # keyword (a virtual method's dispatch thunk takes none).
                cands = cands + ["NoneType"]
            # (None as Optional[NoneType]: the exporter's default for a bare
            # NoneType parameter fails to unpack.)
            lists = [lst + [(p.name, {"pyobj": "pyobj", "NoneType": "Optional[NoneType]"}.get(c, self._cand(c)),
                             "None" if c == "NoneType" else None)]
                     for lst in lists for c in cands]
        return lists

    def _cand(self, c: str) -> str:
        """A GENERIC_PARAMS type name as a type of the generated module."""
        m = re.fullmatch(r"List\[(\w+)\]", c)
        return f"List[_L_{m[1]}]" if m else f"_L_{c}"

    # -- emitting --------------------------------------------------------------

    def fn(self, name: str, params: list[tuple[str, str, str | None]], body: str) -> bool:
        """Emits an exported function, unless an earlier build pruned it."""
        # A default before a parameter without one is dropped (Python's rule;
        # it happens where a left-out generic parameter is passed as None).
        last = max((i for i, p in enumerate(params) if p[2] is None), default=-1)
        params = [(n, t, None if i < last else d) for i, (n, t, d) in enumerate(params)]
        sig = ", ".join(f"{n}: {t}" + (f" = {d}" if d is not None else "") for n, t, d in params)
        key = f"{name}({sig})"
        if key in self.pruned:
            self.last_pruned = self.pruned[key]
            return False
        # Each overload gets its own name: Codon's top-level functions do not
        # overload (a redefinition shadows). The runtime tries them in order.
        names = self.variant_names.setdefault(name, [])
        unique = f"{name}__v{len(names)}"
        names.append(unique)
        self.out.append(f"def {unique}({sig}):\n" + "\n".join("    " + ln for ln in body.splitlines()))
        self.keys.append(key)
        return True

    def emit(self, code: str) -> None:
        self.out.append(code)
        self.keys.append(None)

    @staticmethod
    def kwargs(params, f: Func) -> str:
        """The call's arguments: positional while they match the library's
        parameters in order (a virtual method's dispatch thunk takes no
        keywords), by keyword after a left-out one."""
        out, i = [], 0
        for n, t, _ in params:
            # A Python callable reaches the library as a Codon callable.
            v = f"_PyCallback({n})" if t == "pyobj" else ("None" if t == "Optional[NoneType]" else n)
            if i < len(f.params) and f.params[i].name == n:
                out.append(v)
                i += 1
            else:
                i = len(f.params)
                out.append(f"{n}={v}")
        return ", ".join(out)

    def api_of(self, cls: str) -> dict:
        return self.api.setdefault(cls, {"members": {}, "fields": [], "bases": [], "init": None})

    def conv(self, ann: str | None, var: str, depth: int = 0) -> str:
        """A Codon expression converting variable `var` (of library type
        `ann`) to a `pyobj`.

        Results are converted here, with free functions, rather than by the
        exporter calling `__to_py__`: on a class of a polymorphic hierarchy
        that method is dispatched virtually, and the slot the exporter's
        call needs is empty (it returns NULL).
        """
        if ann is None:
            return f"_pyo({var}.__to_py__())"
        return self._conv(ast.parse(ann, mode="eval").body, var, depth)

    def _conv(self, node, var: str, depth: int) -> str:
        inv = self.inv
        if isinstance(node, ast.Constant) and node.value is None:
            return "_pynone()"
        if isinstance(node, ast.Name):
            n = inv.resolve(node.id)
            if n in ("None", "NoneType"):
                return "_pynone()"
            if n in inv.unions:  # a Union: the member it holds at run time
                expr = "_pynone()"
                for member in reversed(inv.unions[n]):
                    inner = self._conv(ast.Name(member), f"__internal__.union_get_data({var}, _L_{member})", depth + 1)
                    expr = f"({inner} if isinstance({var}, _L_{member}) else {expr})"
                return expr
            if n in PRIMS:
                return f"_pyo({var}.__to_py__())"
            if n in self.public:
                if inv.is_typed_enum(n):
                    return f"_pyo({var}.value.__to_py__())"
                return f"_c_{n}({var})"
            raise Unsupported(f"result type {node.id}")
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name):
            items = node.slice.elts if isinstance(node.slice, ast.Tuple) else [node.slice]
            e = f"_e{depth}"
            kind = node.value.id
            if kind == "List":
                return f"_pyo([{self._conv(items[0], e, depth + 1)} for {e} in {var}].__to_py__())"
            if kind == "Set":
                return f"_pyo([{self._conv(items[0], e, depth + 1)} for {e} in {var}].__to_py__())"
            if kind == "Optional":
                return (f"(_pynone() if {var} is None else "
                        f"{self._conv(items[0], var + '.__val__()', depth + 1)})")
            if kind == "Tuple":
                parts = [self._conv(t, f"{var}[{i}]", depth + 1) for i, t in enumerate(items)]
                return f"_pyo(({', '.join(parts)}{',' if len(parts) == 1 else ''}).__to_py__())"
            if kind == "Dict":
                k, v = f"_k{depth}", f"_v{depth}"
                return (f"_pyo({{{self._conv(items[0], k, depth + 1)}: {self._conv(items[1], v, depth + 1)} "
                        f"for {k}, {v} in {var}.items()}}.__to_py__())")
        raise Unsupported(f"result type {ast.unparse(node)}")

    def converters(self) -> None:
        """`_c_<Class>(x) -> pyobj` for every public class (see conv)."""
        inv = self.inv
        for cls in self.public:
            if inv.is_typed_enum(cls):
                continue
            subs = sorted((s for s in self.public if s != cls and cls in inv.mro(s)
                           and not inv.is_typed_enum(s)), key=lambda s: -len(inv.mro(s)))
            lines = [f"def _c_{cls}(x: _L_{cls}) -> pyobj:"]
            if cls == "Actor":
                lines.append("    return _pyo(_actor_to_py(x))")
            elif cls == "SensorData":
                lines.append("    return _pyo(_sensor_data_to_py(x))")
            else:
                if subs:
                    lines.append("    if static.has_rtti(_L_" + cls + "):")
                    lines.append("        rid = __internal__.to_class_ptr(__internal__.class_raw_rtti_rtti(x), RTTI).id")
                    for sub in subs:
                        lines.append(f"        if rid == _L_{sub}.__id__ and static.has_rtti(_L_{sub}):")
                        lines.append(f"            return _pyo(B_{sub}(__internal__.class_base_to_derived("
                                     f"x, _L_{cls}, _L_{sub}))._tsc_to_py())")
                lines.append(f"    return _pyo(B_{cls}(x)._tsc_to_py())")
            self.emit("\n".join(lines))

    def member(self, cls: str, key: str, fs: list[Func]) -> None:
        self.cls = cls
        name, kind = fs[0].name, fs[0].kind
        fname = f"{cls}__{name}" + ("__set" if kind == "setter" else "")
        selfp = [] if kind == "static" else [("self", f"B_{cls}", None)]
        # Methods are called through their class: `self.v.m(...)` would also
        # see Actor's Python-API shortcuts (_actor_compat), which make the
        # call ambiguous for the exporter ("cannot typecheck").
        target = f"_L_{cls}.{name}" if kind == "static" else f"self.v.{name}"
        # (Special methods keep the instance call: `C.__repr__(x)` is a type's repr.)
        call = (f"_L_{cls}.{name}" if kind == "static" else
                f"self.v.{name}(" if name.startswith("__") else f"_L_{cls}.{name}(self.v")
        emitted, reasons = 0, []
        for f in fs:
            try:
                variants = self.variants(f)
                ret = self.conv(f.ret, "r") if kind != "setter" else None
            except Unsupported as e:
                reasons.append(str(e))
                continue
            for params in variants:
                if kind == "property":
                    body = f"r = {target}\nreturn {ret}"
                elif kind == "setter":
                    body = f"self.v.{name} = {params[0][0]}"
                else:
                    args = self.kwargs(params, f)
                    if kind == "static":
                        expr = f"{call}({args})"
                    elif call.endswith("("):
                        expr = f"{call}{args})"
                    else:
                        expr = f"{call}{', ' + args if args else ''})"
                    body = (f"{expr}\nreturn _pynone()" if f.ret in ("None",) or ret == "_pynone()"
                            else f"r = {expr}\nreturn {ret}")
                if self.fn(fname, selfp + params, body):
                    emitted += 1
                else:
                    reasons.append(f"does not compile: {self.last_pruned}")
        entry = {"fn": fname if emitted else None, "kind": kind}
        if not emitted:
            entry["reason"] = self.stubs[f"{cls}.{name}"] = "; ".join(sorted(set(reasons)))
        self.api_of(cls)["members"][key] = entry

    def klass(self, cls: str) -> None:
        inv = self.inv
        c = inv.classes[cls]
        api = self.api_of(cls)
        # Boxes wrap themselves like Codon's _PyWrap.wrap_to_py, but find
        # their CPython type at run time: _PyWrap.py_type is generated when
        # the exporter gets to the class, after the early realization that
        # fills the vtables (see realize), and would be NULL here. The code
        # calls _tsc_to_py/_tsc_from_py, never __to_py__/__from_py__ on a
        # box: the exporter regenerates those for exported classes, and a
        # call realized against its version returns NULL.
        self.emit(
            f"class B_{cls}:\n    v: _L_{cls}\n"
            f"    def __init__(self, v: _L_{cls}):\n        self.v = v\n"
            f"    def _tsc_to_py(self) -> Ptr[byte]:\n"
            f"        o = Ptr[PyWrapper[B_{cls}]](alloc_uncollectable(sizeof(PyWrapper[B_{cls}])).as_byte())\n"
            f"        o[0] = PyWrapper(PyObject(1, _boxtype({('B_' + cls)!r})), self)\n"
            f"        return o.as_byte()\n"
            f"    def _tsc_from_py(obj: Ptr[byte]) -> B_{cls}:\n"
            f"        w = Ptr[PyWrapper[B_{cls}]](obj)[0]\n"
            f"        if w.head.pytype != _boxtype({('B_' + cls)!r}):\n"
            f"            raise PyError({('expected carla.' + cls)!r})\n"
            f"        return w.data\n"
            f"    def __to_py__(self) -> Ptr[byte]:\n        return self._tsc_to_py()\n"
            f"    def __from_py__(obj: Ptr[byte]) -> B_{cls}:\n        return B_{cls}._tsc_from_py(obj)")
        api["bases"] = [b for b in map(_strip_static, c.bases) if b in self.public]
        if inv.is_typed_enum(cls):
            api["enum"] = {k: int(m[1]) for k, v in c.classvars.items()
                           if (m := re.fullmatch(rf"{cls}\((-?\d+)\)", v))}
            return
        # Constant class attributes (e.g. SensorDataType.Image).
        api["constants"] = {}
        for k, v in c.classvars.items():
            if not k.startswith("_"):
                try:
                    api["constants"][k] = ast.literal_eval(v)
                except (ValueError, SyntaxError):
                    pass
        members = inv.all_members(cls)
        for fname, fann, fmod in inv.all_fields(cls):
            if fname.startswith("_") or fname in members:
                continue
            try:
                t = self.ty(fann)
            except Unsupported as e:
                self.stubs[f"{cls}.{fname}"] = f"field: {e}"
                continue
            self.fn(f"{cls}__{fname}", [("self", f"B_{cls}", None)],
                    f"r = self.v.{fname}\nreturn {self.conv(fann, 'r')}")
            setter = None
            if not c.is_tuple:
                setter = f"{cls}__{fname}__set"
                self.fn(setter, [("self", f"B_{cls}", None), ("value", t, None)],
                        f"self.v.{fname} = value")
            api["fields"].append({"name": fname, "get": f"{cls}__{fname}", "set": setter})
        self.cls = cls
        if "__init__" in c.members:
            emitted, reasons = 0, []
            for f in c.members["__init__"]:
                try:
                    for params in self.variants(f):
                        if self.fn(f"{cls}__new", params,
                                   guarded(f"return B_{cls}(_L_{cls}({self.kwargs(params, f)}))")):
                            emitted += 1
                        else:
                            reasons.append(f"does not compile: {self.last_pruned}")
                except Unsupported as e:
                    reasons.append(str(e))
            api["init"] = f"{cls}__new" if emitted else None
            if not emitted:
                self.stubs[f"{cls}.__init__"] = "; ".join(sorted(set(reasons)))
        elif c.is_tuple:
            try:
                params = [(n, self.ty(a), None) for n, a in c.fields]
                self.fn(f"{cls}__new", params,
                        f"return B_{cls}(_L_{cls}({', '.join(n for n, _, _ in params)}))")
                api["init"] = f"{cls}__new"
            except Unsupported as e:
                self.stubs[f"{cls}.__init__"] = str(e)
        if "__iter__" in members:
            self.fn(f"{cls}____iter__", [("self", f"B_{cls}", None)],
                    guarded("return [x for x in self.v]"))
            api["members"]["__iter__"] = {"fn": f"{cls}____iter__", "kind": "method"}
        for key, fs in members.items():
            name = fs[0].name
            if name in SKIP_MEMBERS or (name.startswith("_") and not name.endswith("__")):
                continue
            self.member(cls, key, fs)

    def conversions(self) -> None:
        """Library objects box on the way to Python and unbox on the way back."""
        inv = self.inv
        for cls in self.public:
            if inv.is_typed_enum(cls):
                self.emit(
                    f"@extend\nclass _L_{cls}:\n"
                    f"    def __to_py__(self) -> Ptr[byte]:\n        return self.value.__to_py__()\n"
                    f"    def __from_py__(obj: Ptr[byte]) -> _L_{cls}:\n"
                    f"        return _L_{cls}(_enum_value(obj))")
                continue
            # A parameter of a class accepts its subclasses, as in Python.
            accepted = [cls] + [s for s in self.public if s != cls and cls in inv.mro(s)
                                and not inv.is_typed_enum(s)]
            # Only a hierarchy's root converts to Python: a subclass's own
            # __to_py__ would override it, and the virtual call made through
            # the exporter's conversion returns NULL. The root dispatches on
            # the object's runtime class (RTTI) to the subclass's box.
            lines = [f"@extend\nclass _L_{cls}:"]
            if not any(b in self.public for b in inv.mro(cls)[1:]):
                subs = sorted((s for s in accepted if s != cls), key=lambda s: -len(inv.mro(s)))
                if cls == "Actor":
                    body = "        return _actor_to_py(self)"
                elif cls == "SensorData":
                    body = "        return _sensor_data_to_py(self)"
                else:
                    # (Inline: a generic top-level helper would be exported.)
                    body = ("        if static.has_rtti(type(self)):\n"
                            "            rid = __internal__.to_class_ptr(__internal__.class_raw_rtti_rtti(self), RTTI).id\n")
                    body += "".join(
                        f"            if rid == _L_{s}.__id__ and static.has_rtti(_L_{s}):\n"
                        f"                return B_{s}(__internal__.class_base_to_derived(self, type(self), _L_{s}))._tsc_to_py()\n"
                        for s in subs) + f"        return B_{cls}(self)._tsc_to_py()"
                    if not subs:
                        body = f"        return B_{cls}(self)._tsc_to_py()"
                lines.append(f"    def __to_py__(self) -> Ptr[byte]:\n{body}")
            lines += [f"    def __from_py__(obj: Ptr[byte]) -> _L_{cls}:",
                      "        t = Ptr[PyObject](obj)[0].pytype"]
            for a in accepted:
                lines.append(f"        if t == _boxtype({('B_' + a)!r}):\n"
                             f"            return B_{a}._tsc_from_py(obj).v")
            lines.append(f"        raise PyError({('expected carla.' + cls)!r})")
            self.emit("\n".join(lines))

    def dispatch(self) -> None:
        """An Actor (SensorData) reaches Python as its concrete class."""
        inv = self.inv
        lines = ["def _actor_to_py(a: _L_Actor) -> Ptr[byte]:"]
        actor = inv.all_members("Actor")
        for kind, cls in (("vehicle", "Vehicle"), ("walker", "Walker"),
                          ("walker_ai_controller", "WalkerAIController"), ("sensor", "Sensor"),
                          ("traffic_light", "TrafficLight"), ("traffic_sign", "TrafficSign")):
            if cls not in self.public or f"as_{kind}" not in actor:
                continue
            if f"is_{kind}" in actor:
                lines.append(f"    if a.is_{kind}():\n        return B_{cls}(a.as_{kind}())._tsc_to_py()")
            else:  # only a checked conversion: try it
                lines.append(f"    try:\n        return B_{cls}(a.as_{kind}())._tsc_to_py()\n"
                             f"    except _E_ActorTypeError:\n        pass")
        lines.append("    return B_Actor(a)._tsc_to_py()")
        self.emit("\n".join(lines))
        # SensorData: its type() is a SensorDataType value; as_<kind>() gives
        # the measurement (Image for SensorDataType.Image, ...).
        data = inv.all_members("SensorData")
        typ = "d.type" if data.get("type", [Func("", "", [], None)])[0].kind == "property" else "d.type()"
        lines = ["def _sensor_data_to_py(d: _L_SensorData) -> Ptr[byte]:", f"    t = {typ}"]
        kinds = inv.classes["SensorDataType"].classvars if "SensorDataType" in inv.classes else {}
        for kind, value in kinds.items():
            for key, fs in data.items():
                ret = inv.resolve(fs[0].ret or "")
                plain = fs[0].name[3:].replace("_", "")   # as_custom_v2x_event -> customv2xevent
                if (fs[0].name.startswith("as_") and plain.startswith(kind.lower())
                        and ret in self.public and not fs[0].params):
                    lines.append(f"    if t == {value}:\n        return B_{ret}(d.{fs[0].name}())._tsc_to_py()")
                    break
        lines.append("    return B_SensorData(d)._tsc_to_py()")
        self.emit("\n".join(lines))

    def functions(self) -> None:
        for name, fs in self.inv.functions.items():
            module = fs[0].module
            if module != "command" and name not in self.inv.public:
                continue
            emitted, reasons = 0, []
            for f in fs:
                try:
                    if f.ret is not None and f.ret != "None":
                        self.ty(f.ret)
                    ret = self.conv(f.ret, "r")
                    for params in self.variants(f):
                        if self.fn(f"_f__{name}", params,
                                   f"r = _M_{module}.{name}({self.kwargs(params, f)})\nreturn {ret}"):
                            emitted += 1
                        else:
                            reasons.append(f"does not compile: {self.last_pruned}")
                except Unsupported as e:
                    reasons.append(str(e))
            entry = {"fn": f"_f__{name}" if emitted else None, "module": module}
            if not emitted:
                entry["reason"] = self.stubs[name] = "; ".join(sorted(set(reasons)))
            self.api.setdefault("__functions__", {})[name] = entry

    def generate(self) -> str:
        inv = self.inv
        modules = sorted({c.module for c in inv.classes.values()}
                         | {f.module for fs in inv.functions.values() for f in fs})
        head = [PRELUDE, f"EXTENSION = {PACKAGE_NAME + '.' + MODULE!r}"]
        head += [f"import typesafe_carla.{m} as _M_{m}" for m in modules if m != "__init__"]
        head += [f"from typesafe_carla import {c} as _L_{c}" for c in self.public]
        head += [f"from typesafe_carla import {e} as _E_{e}" for e in EXCEPTIONS]
        for cls in self.public:
            self.klass(cls)
        self.conversions()
        self.dispatch()
        self.converters()
        self.functions()
        self.realize()
        self.body_start = len("\n".join(head) + "\n\n")
        self.text = self.assemble("\n".join(head) + "\n\n")
        return self.text

    def realize(self) -> None:
        """Realizes every library call the exported functions make, in a
        branch never taken.

        Codon fills the virtual-method tables (`class_populate_vtables`) from
        what is realized while type-checking the module; the exporter
        realizes its functions later, so a virtual call made only from them
        would find an empty slot and crash. So each function's library call
        is realized here first, inside a lambda: realizing the exported
        functions themselves this early breaks the conversion of their
        results (they return NULL). `Ptr[T]()[0]` is an expression of type T
        that is never evaluated.
        """
        calls = []
        self.realize_keys = []    # per call line: the function it stands for
        for code, key in zip(self.out, self.keys):
            if key is None:
                continue
            head, _, body = code.partition("\n")
            m = re.match(r"def (\w+)\((.*)\):", head)
            params = _split_params(m[2])
            first = body.strip().splitlines()[0].strip() if body.strip() else ""
            if first.startswith("r = "):
                expr = first[4:]
            elif first.startswith("return ") or re.match(r"^\w[\w.]*\(", first):
                expr = first.removeprefix("return ")
            else:
                continue
            for q in params:
                pname, ptype = q.split(":", 1)[0].strip(), q.split(":", 1)[1].split("=")[0].strip()
                expr = re.sub(rf"(?<![\w.]){pname}\b", f"Ptr[{ptype}]()[0]", expr)
            calls.append(f"    {expr}")
            self.realize_keys.append(key)
        # The conversions' own library calls (actor kinds, sensor data kinds).
        for code in self.out:
            if code.startswith(("def _actor_to_py", "def _sensor_data_to_py")):
                arg = "Ptr[_L_Actor]()[0]" if "_actor_to_py" in code else "Ptr[_L_SensorData]()[0]"
                var = "a" if "_actor_to_py" in code else "d"
                for m in re.finditer(rf"\b{var}\.(\w+)\(\)", code):
                    calls.append(f"    {arg}.{m[1]}()")
        # (In a function: top-level code and lambdas are exported too.)
        self.out.append(REALIZE + ("\n".join(calls) or "    pass")
                        + "\nif len(_sys.argv) < 0:\n    _realize_library_calls()")
        self.keys.append(None)

    def assemble(self, head: str) -> str:
        """head + the chunks; records each generated function's line range."""
        parts = [head]
        line = head.count("\n") + 1
        self.lines = []
        for code, key in zip(self.out, self.keys):
            n = code.count("\n") + 1
            if key is not None:
                self.lines.append((line, line + n - 1, key))
            elif code.startswith(REALIZE):
                # A realized library call stands for its function.
                first = line + REALIZE.count("\n")
                self.lines += [(first + i, first + i, k) for i, k in enumerate(self.realize_keys)]
            parts.append(code + "\n\n")
            line += n + 1
        return "".join(parts)


PRELUDE = '''# Generated by tools/pycarla.py from codon/typesafe_carla; do not edit.
import typesafe_carla as _tc
from internal.python import _PyWrap, PyError, PyObject
from internal.gc import alloc_uncollectable
import internal.python as _ipy
import internal.static as static

def _runtime_error() -> pyobj:
    return pyobj(_ipy.PyExc_RuntimeError, steal=False)

_box_types = Dict[str, Ptr[byte]]()

def _boxtype(name: str) -> Ptr[byte]:
    """The CPython type of box class `name`, from the loaded extension module."""
    if name not in _box_types:
        _box_types[name] = pyobj._import(EXTENSION)._getattr(name).p
    return _box_types[name]

def _pyo(p: Ptr[byte]) -> pyobj:
    return pyobj(p, steal=True)

def _pynone() -> pyobj:
    return pyobj(_ipy.Py_None, steal=False)

def _enum_value(obj: Ptr[byte]) -> int:
    return int(pyobj(obj, steal=False))

@extend
class Ptr:
    def __to_py__(self) -> Ptr[byte]:
        return self.as_byte()
    def __from_py__(o: Ptr[byte]) -> Ptr[T]:
        return Ptr[T](o)

class _PyCallback:
    """A Python callable, called by the library with its own objects
    (converted to Python by their __to_py__)."""
    f: pyobj
    def __init__(self, f: pyobj):
        self.f = f
    def __call__(self, data):
        self.f(data)
'''


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def generate(out: Path, pruned: dict[str, str] | None = None) -> tuple[Path, Gen]:
    gen = Gen(Inventory(), pruned)
    source = out / f"{MODULE}.codon"
    source.write_text(gen.generate())
    return source, gen


ANSI = re.compile(r"\x1b\[[0-9;]*m")
ERROR = re.compile(r"^[\s│├╰─]*(?P<file>[^\s:]+):(?P<line>\d+)(?: \([0-9-]+\))?: error: (?P<msg>.*)$")


def _culprits(stderr: str, gen: Gen) -> dict[str, str]:
    """Generated functions whose realization a compiler error passes through."""
    chains: list[list[tuple[str, int, str]]] = []
    for raw in ANSI.sub("", stderr).splitlines():
        m = ERROR.match(raw)
        if not m:
            continue
        entry = (m["file"], int(m["line"]), m["msg"].strip())
        if raw.lstrip()[:1] in ("├", "╰", "│") and chains:
            chains[-1].append(entry)
        else:
            chains.append([entry])
    out = {}
    names = {code.split("(", 1)[0][4:]: key for code, key in zip(gen.out, gen.keys) if key}
    for chain in chains:
        for f, ln, msg in reversed(chain):   # the outermost generated function
            # The exporter's wrapper names the function it wraps.
            m = re.search(r"F: '(\w+)\.\d+:\d+'", msg)
            if m and m[1] in names:
                out[names[m[1]]] = chain[0][2]
                break
            if f != f"{MODULE}.codon":
                continue
            hit = [k for a, b, k in gen.lines if a <= ln <= b]
            if hit:
                out[hit[0]] = chain[0][2]
                break
    return out


def _compile(source: Path, output: Path, env: dict[str, str] | None,
             llvm: bool = False) -> subprocess.CompletedProcess:
    output.unlink(missing_ok=True)
    cmd = [sys.executable, "-m", "typesafe_carla.cli", "build", "--pyext", "--relocation-model=pic",
           "--module", MODULE] + (["--llvm"] if llvm else []) + ["-o", str(output), str(source)]
    return subprocess.run(cmd, capture_output=True, text=True,
                          env={**os.environ, "TYPESAFE_CARLA_COMPAT_WARNINGS": "0", **(env or {})})


def _probe(out: Path, gen: Gen, shards: int, env: dict[str, str] | None) -> dict[str, str]:
    """Type-checks the generated functions in `shards` parallel compiles, each
    with every shared definition and one share of the functions; returns the
    failing functions (one per shard and round, as Codon stops at an error)."""
    import concurrent.futures

    import threading

    lock = threading.Lock()
    keyed = [i for i, k in enumerate(gen.keys) if k is not None]
    head = gen.text[:gen.body_start]

    def one(n: int):
        mine = set(keyed[n::shards])
        keep = [i for i, c in enumerate(gen.out)
                if (gen.keys[i] is None or i in mine) and not c.startswith(REALIZE)]
        sub = Gen(gen.inv, gen.pruned)
        sub.out, sub.keys = [gen.out[i] for i in keep], [gen.keys[i] for i in keep]
        sub.realize()
        text = sub.assemble(head)
        d = out / "probe" / str(n)
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{MODULE}.codon").write_text(text)
        r = _compile(d / f"{MODULE}.codon", d / f"{MODULE}.o", env)
        if r.returncode in (-9, 137):  # killed (out of memory): once more, alone
            with lock:
                r = _compile(d / f"{MODULE}.codon", d / f"{MODULE}.o", env)
        return {} if r.returncode == 0 else (_culprits(r.stderr, sub) or {"": r.stderr[-3000:]})

    found: dict[str, str] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=shards) as pool:
        for culprits in pool.map(one, range(shards)):
            found.update(culprits)
    return found


def compile_module(out: Path, env: dict[str, str] | None = None, log=print, shards: int = 4) -> Gen:
    """Generates and compiles `_carla.o`, pruning variants that do not compile.

    An untyped library parameter is exported as one overload per type it may
    take (GENERIC_PARAMS); a combination the library rejects (its own
    compile-time checks) is a compile error. Such variants are dropped,
    recorded in compat/pycarla/pruned.json, and the module is compiled again
    (sharded type checks find several per round).
    """
    pruned = json.loads(PRUNED.read_text()) if PRUNED.is_file() else {}
    obj = out / f"{MODULE}.o"
    for _ in range(200):
        source, gen = generate(out, pruned)
        result = _compile(source, obj, env)
        if result.returncode == 0 and obj.is_file():
            return gen
        culprits = _culprits(result.stderr, gen)
        if not culprits:
            raise RuntimeError(f"pyext build failed (exit {result.returncode}):\n{result.stderr[-6000:]}")
        while culprits:
            if "" in culprits:
                raise RuntimeError(f"pyext build failed in a shared definition:\n{culprits['']}")
            for key, msg in culprits.items():
                log(f"pycarla: pruned {key}: {msg}")
                pruned[key] = msg
            PRUNED.write_text(json.dumps(pruned, indent=1, sort_keys=True) + "\n")
            _, gen = generate(out, pruned)
            culprits = _probe(out, gen, shards, env)
    raise RuntimeError("pyext build: too many pruning rounds")


def write_package(pkg: Path, gen: Gen) -> None:
    """carla/__init__.py, its runtime, and _spec.json (what the runtime builds)."""
    inv = gen.inv
    spec = {
        "classes": {c: gen.api[c] for c in gen.public if c in gen.api},
        "functions": gen.api.get("__functions__", {}),
        "aliases": {n: inv.resolve(n) for n in inv.public if inv.resolve(n) != n},
        "int_enums": {n: inv.int_enums[n] for n in inv.public if n in inv.int_enums},
        "command": inv.command,
        "command_constants": inv.constants.get("command", {}),
        "exceptions": EXCEPTIONS,
        "stubs": gen.stubs,
        "variants": gen.variant_names,
        "native_library": str(_native_library()),
    }
    pkg.mkdir(parents=True, exist_ok=True)
    (pkg / "_spec.json").write_text(json.dumps(spec, indent=1, sort_keys=True))
    (pkg / "_runtime.py").write_text(RUNTIME.read_text())
    (pkg / "__init__.py").write_text(
        '"""carla: typesafe_carla as a CPython package (generated by tools/pycarla.py)."""\n'
        "from ._runtime import install as _install\n_install(globals())\ndel _install\n")


def build(out: Path, env: dict[str, str] | None = None) -> Path:
    """Builds the `carla` package into `out`; returns `out`."""
    from typesafe_carla import toolchain

    out.mkdir(parents=True, exist_ok=True)
    gen = compile_module(out, env)
    libdirs = [str(d) for d in toolchain.find_codon().library_dirs()]
    pkg = out / "carla"
    pkg.mkdir(parents=True, exist_ok=True)
    link = ["cc", "-shared", "-o", str(pkg / f"{MODULE}.so"), str(out / f"{MODULE}.o")]
    link += [f"-L{d}" for d in libdirs] + ["-lcodonrt"] + [f"-Wl,-rpath,{d}" for d in libdirs]
    subprocess.run(link, check=True)
    write_package(pkg, gen)
    return out


def _native_library() -> Path:
    from typesafe_carla import paths

    return paths.native_library()


def default_out() -> Path:
    from typesafe_carla import paths

    return paths.native_library().parent / "pycarla"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.pycarla",
                                     description=__doc__.split("\n\n")[0])
    parser.add_argument("-o", "--out", type=Path, help="output directory (default <build dir>/pycarla)")
    parser.add_argument("--report", action="store_true",
                        help="only generate, and list the members that are not wrapped")
    args = parser.parse_args(argv)
    out = (args.out or default_out()).resolve()
    if args.report:
        out.mkdir(parents=True, exist_ok=True)
        _, gen = generate(out)
        write_package(out / "carla", gen)
        for k, v in sorted(gen.stubs.items()):
            print(f"{k}: {v}")
        return 0
    build(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
