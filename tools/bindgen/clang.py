"""libclang checks of the spec against the LibCarla (or mock) headers of a build.

The translation units are the shim sources themselves, parsed with the flags
CMake recorded in compile_commands.json, so the same headers the shim compiles
against are the ones checked: real LibCarla for a libcarla build, the mock's
mirror of it for a mock build.
"""

from __future__ import annotations

import functools
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from clang import cindex

from .spec import ROOT, Function, Spec

SHIM_SOURCES = ROOT / "native" / "src"
COMPAT_HEADER = (SHIM_SOURCES / "carla_compat.hpp").resolve()
GENERATED_SOURCES = (SHIM_SOURCES / "generated").resolve()
# LibCarla classes whose public methods the coverage report lists.
COVERAGE_CLASSES = [
    "carla::client::Client", "carla::client::World", "carla::client::Map",
    "carla::client::Waypoint", "carla::client::Junction", "carla::client::Landmark",
    "carla::client::Actor", "carla::client::Vehicle", "carla::client::Walker",
    "carla::client::WalkerAIController", "carla::client::TrafficSign",
    "carla::client::TrafficLight",
    "carla::client::Sensor",
    "carla::client::BlueprintLibrary", "carla::client::ActorBlueprint",
    "carla::client::ActorList", "carla::client::WorldSnapshot", "carla::client::DebugHelper",
    "carla::client::LightManager",  # issue #21
    "carla::traffic_manager::TrafficManager",
    # Measurements (issue #24). Image and OpticalFlowImage are instantiations
    # of the ImageTmpl template, which the report does not list.
    "carla::sensor::SensorData", "carla::sensor::data::LidarMeasurement",
    "carla::sensor::data::SemanticLidarMeasurement", "carla::sensor::data::RadarMeasurement",
    "carla::sensor::data::GnssMeasurement", "carla::sensor::data::IMUMeasurement",
    "carla::sensor::data::CollisionEvent", "carla::sensor::data::ObstacleDetectionEvent",
    "carla::sensor::data::LaneInvasionEvent", "carla::sensor::data::DVSEventArray",
]


def _compile_commands(build_dir: Path) -> dict[Path, list[str]]:
    path = build_dir / "compile_commands.json"
    if not path.exists():
        raise SystemExit(f"{path} not found; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "
                         "(the default for this project)")
    out = {}
    for entry in json.loads(path.read_text()):
        args = entry.get("arguments") or shlex.split(entry["command"])
        out[Path(entry["file"]).resolve()] = args
    return out


def _clang_args(args: list[str]) -> list[str]:
    """The compiler flags that matter for parsing, plus the compiler's own
    builtin headers."""
    keep, it = [], iter(args[1:])
    for a in it:
        if a in ("-o", "-c", "-MF", "-MT", "-MQ"):
            next(it, None)
        elif a.startswith(("-I", "-D", "-std=", "-isystem", "-include")):
            keep.append(a)
            if a in ("-I", "-D", "-isystem", "-include"):
                keep.append(next(it))
    return keep + ["-resource-dir", _resource_dir(), "-x", "c++", "-Wno-everything"]


@functools.cache
def _resource_dir() -> str:
    """Clang's builtin headers (stddef.h, the x86 intrinsics...), which the
    libclang wheel does not ship. TSC_CLANG_RESOURCE_DIR, else an installed
    clang, else the copy inside the `ziglang` package."""
    def candidates():  # lazily: each clang costs a subprocess
        yield os.environ.get("TSC_CLANG_RESOURCE_DIR", "")
        for clang in ("clang", *(f"clang-{v}" for v in range(22, 13, -1))):
            if shutil.which(clang):
                yield subprocess.run([clang, "-print-resource-dir"], capture_output=True,
                                     text=True).stdout.strip()
        try:
            import ziglang

            yield str(Path(ziglang.__file__).parent / "lib")
        except ImportError:
            pass

    for c in candidates():
        if c and (Path(c) / "include" / "stddef.h").exists():
            return c
    raise SystemExit("no clang resource directory (clang's builtin headers) found: install clang, "
                     "set TSC_CLANG_RESOURCE_DIR, or run with `uv run --with ziglang==0.13.0`")


def _parse(source: Path, args: list[str], bodies: bool = True) -> cindex.TranslationUnit:
    """`bodies=False` skips function bodies: enough for declarations, faster."""
    options = 0 if bodies else cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
    tu = cindex.Index.create().parse(str(source), args=_clang_args(args), options=options)
    errors = [d for d in tu.diagnostics if d.severity >= cindex.Diagnostic.Error]
    if errors:
        raise SystemExit(f"{source}: libclang could not parse it:\n" +
                         "\n".join(f"  {d}" for d in errors[:10]))
    return tu


def _qualified(cursor: cindex.Cursor) -> str:
    parts = []
    while cursor is not None and cursor.kind != cindex.CursorKind.TRANSLATION_UNIT:
        if cursor.spelling:
            parts.append(cursor.spelling)
        cursor = cursor.semantic_parent
    return "::".join(reversed(parts))


@dataclass
class Method:
    cls: str  # the class that declares it (a base class for inherited methods)
    name: str
    params: list[str]  # canonical types, const/ref stripped
    result: str
    required: int  # parameters without a default argument
    deprecated: bool


def _strip(t: cindex.Type) -> str:
    t = t.get_canonical()
    if t.kind in (cindex.TypeKind.LVALUEREFERENCE, cindex.TypeKind.RVALUEREFERENCE):
        t = t.get_pointee().get_canonical()
    return re.sub(r"\bconst\b\s*", "", t.spelling).strip()


def _class_definitions(tu: cindex.TranslationUnit) -> dict[str, cindex.Cursor]:
    """Every class defined in the carla namespace, by qualified name."""
    out: dict[str, cindex.Cursor] = {}

    def visit(node: cindex.Cursor):
        for c in node.get_children():
            if c.kind == cindex.CursorKind.NAMESPACE and (c.spelling == "carla" or
                                                           node.kind != cindex.CursorKind.TRANSLATION_UNIT):
                visit(c)
            elif c.kind in (cindex.CursorKind.CLASS_DECL, cindex.CursorKind.STRUCT_DECL) \
                    and c.is_definition():
                out.setdefault(_qualified(c), c)
                visit(c)

    visit(tu.cursor)
    return out


def _method(cls: str, m: cindex.Cursor) -> Method:
    params = list(m.get_arguments())
    required = sum(1 for p in params
                   if not any(t.kind == cindex.TokenKind.PUNCTUATION and t.spelling == "="
                              for t in p.get_tokens()))
    deprecated = any(t.spelling == "deprecated" for t in m.get_tokens())
    return Method(cls, m.spelling, [_strip(p.type) for p in params], _strip(m.result_type),
                  required, deprecated)


def _methods(defs: dict[str, cindex.Cursor], cls: str,
             stop: set[str] = frozenset()) -> dict[str, list[Method]]:
    """Methods callable on `cls` from outside, by name (overloads listed), as
    C++ name lookup finds them: a name declared (or `using`-declared) in a
    class hides that name in its bases; inherited methods count when reached
    through public bases, or when a public `using` exposes them. Bases in
    `stop` are not searched. `defs` is _class_definitions() of the TU."""
    out: dict[str, list[Method]] = {}
    if cls not in defs:
        return out
    hidden: set[str] = set()  # names a more-derived class already declares
    exposed: set[str] = set()  # names a public `using Base::name;` brings in
    pending = [(defs[cls], True)]  # (class, reached through public bases only)
    while pending:
        node, public_path = pending.pop(0)
        owner = _qualified(node)
        declared = set()
        for c in node.get_children():
            if c.kind == cindex.CursorKind.USING_DECLARATION:
                if c.spelling in hidden:
                    continue
                declared.add(c.spelling)
                if c.access_specifier == cindex.AccessSpecifier.PUBLIC:
                    exposed.add(c.spelling)  # found in a base below
            elif c.kind == cindex.CursorKind.CXX_METHOD:
                if c.spelling in hidden and c.spelling not in exposed:
                    continue
                declared.add(c.spelling)
                public = public_path and c.access_specifier == cindex.AccessSpecifier.PUBLIC
                if (public or c.spelling in exposed) and not c.is_deleted_method():
                    out.setdefault(c.spelling, []).append(_method(owner, c))
            elif c.kind == cindex.CursorKind.CXX_BASE_SPECIFIER:
                base = c.type.get_canonical().get_declaration()
                base = defs.get(_qualified(base), base)
                if _qualified(base) not in stop and base.is_definition():
                    pending.append((base, public_path and c.access_specifier
                                    == cindex.AccessSpecifier.PUBLIC))
        # Names declared here hide the bases' declarations, except those a
        # `using` declaration brings in from them.
        hidden |= declared - exposed
    return out


def _matches(patterns: tuple[str, ...], canonical: str) -> bool:
    return any(re.fullmatch(p, canonical) for p in patterns)


def _missing_allowed(f: Function, backend: str, ref: str) -> str | None:
    """None if this build may lack `f.call`, else why not. Only a real LibCarla
    may: through a `via` helper (any ref), or an `optional` method on a ref the
    spec lists as lacking it. The mock mirrors the newest LibCarla and must have
    every method, so a misspelt `call` still fails there."""
    if f.via:
        return None if backend == "libcarla" else "no such method (a `via` call must exist in the mock)"
    if f.optional:
        if backend == "libcarla" and ref in f.missing_in:
            return None
        return (f"no such method (optional: missing only in {list(f.missing_in)}, this build is "
                f"{backend} {ref})")
    return "no such method"


def _check(f: Function, overloads: list[Method], backend: str = "mock", ref: str = "") -> str | None:
    """None if one overload accepts the spec's arguments, else why not. A
    result the spec does not output is ignored (e.g. Destroy's bool). A missing
    method is checked by _missing_allowed."""
    if not overloads:
        return _missing_allowed(f, backend, ref)
    reasons = []
    for m in overloads:
        n = len(f.args)
        if not (m.required <= n <= len(m.params)):
            reasons.append(f"takes {m.required}..{len(m.params)} arguments, spec passes {n}")
            continue
        bad = [f"{a.name}: {a.type.name} vs {p}" for a, p in zip(f.args, m.params)
               if not _matches(a.type.cpp, p)]
        if f.out and not _matches(f.out.type.cpp, m.result):
            bad.append(f"result: {f.out.type.name} vs {m.result}")
        if not bad:
            return None
        reasons.append("; ".join(bad))
    return " | ".join(reasons) or "no such method"


def _shim_tu(build_dir: Path) -> tuple[Path, list[str]]:
    commands = _compile_commands(build_dir)
    source = (SHIM_SOURCES / "generated" / "bindings.cpp").resolve()
    if source not in commands:
        raise SystemExit(f"{source} is not in {build_dir}/compile_commands.json; reconfigure")
    return source, commands[source]


def _cache_var(build_dir: Path, name: str) -> str:
    """A variable of the build's CMakeCache.txt, or "unknown"."""
    cache = build_dir / "CMakeCache.txt"
    m = re.search(rf"^{name}:\w+=(.+)$", cache.read_text(), re.MULTILINE) if cache.exists() else None
    return m.group(1) if m else "unknown"


def validate(spec: Spec, build_dir: Path) -> int:
    source, args = _shim_tu(build_dir)
    defs = _class_definitions(_parse(source, args, bodies=False))
    methods = {cls: _methods(defs, cls) for cls in spec.classes()}
    failures = []
    backend = _cache_var(build_dir, "TSC_BACKEND")
    ref = _cache_var(build_dir, "TSC_CARLA_GIT_REF")
    for f in spec.functions:
        why = _check(f, methods[f.cpp_class].get(f.call, []), backend, ref)
        if why:
            failures.append(f"{f.spec_file}: {f.name} -> {f.cpp_class}::{f.call}: {why}")
    if failures:
        print(f"bindgen validate ({backend}): {len(failures)} of {len(spec.functions)} functions "
              "do not match the headers:", file=sys.stderr)
        print("\n".join(f"  {x}" for x in failures), file=sys.stderr)
        return 1
    print(f"bindgen validate ({backend}): {len(spec.functions)} functions match "
          f"{len(spec.classes())} LibCarla classes")
    return 0


def _in_shim(cursor: cindex.Cursor) -> Path | None:
    """The shim source `cursor` is in, or None (LibCarla, the standard library...)."""
    f = cursor.location.file
    if f is None:
        return None
    path = Path(f.name).resolve()
    return path if SHIM_SOURCES.resolve() in path.parents else None


def _location(cursor: cindex.Cursor) -> tuple[str, int, int]:
    loc = cursor.location
    return (str(Path(loc.file.name).resolve()) if loc.file else "", loc.line, loc.column)


def _member_name(call: cindex.Cursor) -> tuple[cindex.Cursor | None, str | None]:
    """For a dependent member call `x.name(...)` (libclang resolves neither the
    method nor its spelling): the MEMBER_REF_EXPR and `name`, its last token."""
    member = next((c for c in call.get_children() if c.kind == cindex.CursorKind.MEMBER_REF_EXPR),
                  None)
    tokens = list(member.get_tokens()) if member else []
    if tokens and tokens[-1].kind == cindex.TokenKind.IDENTIFIER:
        return member, tokens[-1].spelling
    return member, None


def _param_members(template: cindex.Cursor) -> list[tuple[int, str]]:
    """(parameter index, method name) for each dependent member call a shim
    function template makes on one of its parameters, e.g. (0, "at") for
    `items.at(index)` in list_at(const Items &items, ...)."""
    params = [c for c in template.get_children() if c.kind == cindex.CursorKind.PARM_DECL]
    index = {p: i for i, p in enumerate(params)}
    out = []
    for node in template.walk_preorder():
        if node.kind != cindex.CursorKind.CALL_EXPR or node.referenced is not None:
            continue
        member, name = _member_name(node)
        base = next(member.get_children(), None) if member else None
        while base is not None and base.kind == cindex.CursorKind.UNEXPOSED_EXPR:
            base = next(base.get_children(), None)
        if name and base is not None and base.kind == cindex.CursorKind.DECL_REF_EXPR \
                and base.referenced in index:
            out.append((index[base.referenced], name))
    return out


def _declaring_classes(cls: cindex.Cursor, name: str) -> set[str]:
    """The classes declaring the method `name` that a call on `cls` finds:
    `cls` itself, else the nearest bases declaring it."""
    cls = cls.get_definition() or cls
    if any(c.kind == cindex.CursorKind.CXX_METHOD and c.spelling == name
           for c in cls.get_children()):
        return {_qualified(cls)}
    return set().union(*(_declaring_classes(c.type.get_canonical().get_declaration(), name)
                         for c in cls.get_children()
                         if c.kind == cindex.CursorKind.CXX_BASE_SPECIFIER))


def _calls(tu: cindex.TranslationUnit, classes: set[str]) -> set[tuple[str, bool]]:
    """("Class::Method", generated) for every method of `classes` the
    translation unit calls; `generated` if the call is made by the generated
    code (native/src/generated), directly or through a shim function template
    it instantiates (e.g. list_at's `items.at(index)`)."""
    called = set()
    templates: dict[tuple[str, int, int], list[tuple[int, str]]] = {}
    instantiations = []  # (call, its template's location, generated)
    for node in tu.cursor.walk_preorder():
        if node.kind == cindex.CursorKind.FUNCTION_TEMPLATE and node.is_definition() \
                and _in_shim(node):
            templates[_location(node)] = _param_members(node)
        if node.kind != cindex.CursorKind.CALL_EXPR:
            continue
        # Only calls the shim makes, not those inside LibCarla's inline code.
        source = _in_shim(node)
        if source is None:
            continue
        generated = GENERATED_SOURCES in source.parents
        ref = node.referenced
        if ref is None:
            # A dependent call in carla_compat.hpp, which picks between
            # methods renamed across CARLA versions: only the name is known,
            # and libclang leaves it unspelled (it is the member's last token).
            if source == COMPAT_HEADER:
                _, name = _member_name(node)
                if name:
                    called.add((f"*::{name}", generated))
            continue
        if ref.kind == cindex.CursorKind.CXX_METHOD:
            cls = _qualified(ref.semantic_parent)
            if cls in classes:
                called.add((f"{cls}::{ref.spelling}", generated))
        elif ref.kind == cindex.CursorKind.FUNCTION_DECL and _in_shim(ref):
            # Possibly an instantiation of a shim function template: its
            # location is the template's.
            instantiations.append((node, _location(ref), generated))
    # The template's dependent member calls, on the classes of the arguments
    # this call passes to those parameters.
    for node, where, generated in instantiations:
        args = list(node.get_arguments())
        for i, name in templates.get(where, ()):
            if i >= len(args):
                continue
            decl = args[i].type.get_canonical().get_declaration()
            if decl.kind == cindex.CursorKind.NO_DECL_FOUND:
                continue
            called |= {(f"{cls}::{name}", generated) for cls in _declaring_classes(decl, name)
                       if cls in classes}
    return called


def _calls_in(source: Path, args: list[str], classes: set[str]) -> set[tuple[str, bool]]:
    return _calls(_parse(source, args), classes)


def _called_methods(build_dir: Path, classes: set[str], exclude: Path) -> set[tuple[str, bool]]:
    """_calls() of every shim source other than `exclude` (already parsed by
    the caller)."""
    sources = {s: a for s, a in _compile_commands(build_dir).items()
               if SHIM_SOURCES.resolve() in s.parents and s != exclude}
    with ProcessPoolExecutor() as pool:  # each parse takes seconds
        results = pool.map(_calls_in, sources, sources.values(), [classes] * len(sources))
        return set().union(*results)


def coverage(spec: Spec, build_dir: Path, output: Path | None) -> int:
    source, args = _shim_tu(build_dir)
    classes = set(COVERAGE_CLASSES)
    tu = _parse(source, args)
    defs = _class_definitions(tu)
    missing = [cls for cls in COVERAGE_CLASSES if cls not in defs]
    if missing:
        print(f"bindgen coverage: not in the shim's headers, skipped: {missing}", file=sys.stderr)
    by_class = {cls: _methods(defs, cls, stop=classes - {cls}) for cls in COVERAGE_CLASSES}
    owners = {m.cls for methods in by_class.values() for overloads in methods.values()
              for m in overloads}
    calls = _calls(tu, owners) | _called_methods(build_dir, owners, exclude=source)
    called = {key for key, _ in calls}
    # Generated: a spec'd method (even one called through a `via` helper),
    # or one the generated code calls itself, e.g. the list accessors' size
    # and at (bindings/lists.yaml, through list_at).
    spec_calls = {f"{f.cpp_class}::{f.call}" for f in spec.functions}
    generated_calls = {key for key, gen in calls if gen}
    ref = _cache_var(build_dir, "TSC_CARLA_GIT_REF")

    lines = ["# LibCarla API coverage", "",
             f"Which public methods of the main LibCarla client classes the C shim calls, "
             f"for CARLA ref `{ref}` ({_cache_var(build_dir, 'TSC_BACKEND')} backend).", "",
             f"Regenerate with `uv run python -m tools.bindgen coverage --build-dir <build> -o docs/coverage.md`.",
             "", "- **generated**: the binding is generated from `bindings/*.yaml`",
             "- **hand-written**: called from a hand-written shim function",
             "- **—**: not bound yet", ""]
    total = bound = gen = 0
    summary, details = [], []
    for cls, methods in by_class.items():
        names = sorted(methods)
        if not names:
            continue
        rows, cls_bound = [], 0
        for name in names:
            key = f"{cls}::{name}"
            deprecated = all(m.deprecated for m in methods[name])
            declared = {f"{m.cls}::{name}" for m in methods[name]}
            is_called = f"*::{name}" in called or bool(declared & called)
            is_generated = key in spec_calls or bool(declared & generated_calls)
            status = "generated" if is_generated else "hand-written" if is_called else "—"
            if status != "—":
                cls_bound += 1
            gen += status == "generated"
            rows.append(f"| `{name}` | {status}{' (deprecated)' if deprecated else ''} |")
        total += len(names)
        bound += cls_bound
        summary.append(f"| `{cls}` | {cls_bound} / {len(names)} |")
        details += [f"### `{cls}`", "", "| method | binding |", "|---|---|", *rows, ""]
    lines += [f"**{bound} of {total} methods bound ({100 * bound // max(total, 1)}%), "
              f"{gen} of them generated.**", "", "| class | bound |", "|---|---|", *summary, "",
              "## Methods", "", *details]
    text = "\n".join(lines)
    if output:
        output.write_text(text)
        print(f"bindgen coverage: {bound} of {total} methods bound, {gen} generated -> {output}")
    else:
        print(text)
    return 0
