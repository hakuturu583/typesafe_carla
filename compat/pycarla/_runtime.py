"""Runtime of the generated `carla` package (tools/pycarla.py).

`_carla.so` exports one function per typesafe_carla member, taking and
returning opaque boxes (`B_<Class>`). This module builds the CARLA Python
API's classes from `_spec.json`. Each instance holds a box in `_o`. Methods
forward to the extension and convert boxes to Python objects and back. A
member the generator could not wrap raises NotImplementedError and says why.

Generic, with no per-class code. Python classes give what the extension
cannot: inheritance (`isinstance(vehicle, carla.Actor)`), enumerations,
`carla.command`, and callbacks receiving wrapped objects.

Environment:
  TYPESAFE_CARLA_LIB      the native library (default: the one built against);
  TSC_PYCARLA_REDIRECT    host:port; every carla.Client connects there instead
                          (the upstream-test harness uses it to point tests that
                          hard-code their address at the test server).
"""

from __future__ import annotations

import enum
import json
import os
import sys
import types
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SPEC = json.loads((_HERE / "_spec.json").read_text())
if not os.environ.get("TYPESAFE_CARLA_LIB") and _SPEC.get("native_library"):
    os.environ["TYPESAFE_CARLA_LIB"] = _SPEC["native_library"]

from . import _carla as _ext  # noqa: E402  (needs TYPESAFE_CARLA_LIB)

_BOXES: dict[type, type] = {}


class _Overloads:
    """An exported function's overloads (`<name>__v<i>`), tried in order."""

    __slots__ = ("name", "fns")

    def __init__(self, name: str):
        self.name = name
        self.fns = [getattr(_ext, n) for n in _SPEC["variants"].get(name, [])]

    def __call__(self, *args, **kwargs):
        for f in self.fns:
            try:
                return f(*args, **kwargs)
            except TypeError as e:
                if not str(e).startswith("could not find callable method"):
                    raise
        raise TypeError(f"could not find callable method '{self.name}' for given arguments "
                        f"{[type(a).__name__ for a in args]} {sorted(kwargs)}")


def _fn(name: str) -> _Overloads:
    return _Overloads(name)


class CarlaObject:
    """Base of every generated class; `_o` is the extension's box."""

    __slots__ = ("_o",)


def _wrap(v):
    """An extension result as Python objects."""
    cls = _BOXES.get(type(v))
    if cls is not None:
        obj = cls.__new__(cls)
        obj._o = v
        return obj
    if type(v) is list:
        return [_wrap(x) for x in v]
    if type(v) is tuple:
        return tuple(_wrap(x) for x in v)
    if type(v) is dict:
        return {_wrap(k): _wrap(x) for k, x in v.items()}
    return v


def _unwrap(v):
    """A Python argument as the extension takes it."""
    if isinstance(v, CarlaObject):
        return v._o
    if isinstance(v, enum.Enum):
        return int(v)
    if type(v) in (list, tuple):
        return type(v)(_unwrap(x) for x in v)
    if isinstance(v, (bytes, bytearray, memoryview)) or type(v).__name__ == "array":
        return list(v)
    if isinstance(v, dict):
        return {k: _unwrap(x) for k, x in v.items()}
    if callable(v) and not isinstance(v, type):
        return lambda *a: _unwrap(v(*(_wrap(x) for x in a)))
    return v


def _call(fn, args, kwargs):
    try:
        return _wrap(fn(*(_unwrap(a) for a in args), **{k: _unwrap(x) for k, x in kwargs.items()}))
    except BaseException as e:
        # typesafe_carla's errors (CarlaError, TimeoutError, ...) have no
        # CPython type and arrive as plain BaseException; CARLA's Python API
        # raises RuntimeError.
        if type(e) is BaseException:
            raise RuntimeError(str(e)) from None
        raise


def _stub(qualname: str, reason: str):
    def stub(*args, **kwargs):
        raise NotImplementedError(f"pycarla: {qualname} not wrapped: {reason}")
    stub.__name__ = qualname.rsplit(".", 1)[-1]
    return stub


_BINARY = {"__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__", "__add__", "__sub__",
           "__mul__", "__truediv__", "__radd__", "__rsub__", "__rmul__", "__rtruediv__"}


def _method(cls: str, name: str, entry: dict):
    if entry.get("fn") is None:
        return _stub(f"{cls}.{name}", entry.get("reason", "unknown"))
    fn = _fn(entry["fn"])

    def method(self, *args, **kwargs):
        try:
            return _call(fn, (self,) + args, kwargs)
        except TypeError:
            if name in _BINARY:
                return NotImplemented
            raise

    method.__name__ = name
    method.__qualname__ = f"{cls}.{name}"
    return method


def _static(cls: str, name: str, entry: dict):
    if entry.get("fn") is None:
        return staticmethod(_stub(f"{cls}.{name}", entry.get("reason", "unknown")))
    fn = _fn(entry["fn"])
    return staticmethod(lambda *a, **k: _call(fn, a, k))


def _property(cls: str, name: str, getter: dict, setter: dict | None):
    fget = _method(cls, name, getter)
    fset = None
    if setter is not None:
        f = _method(cls, name, setter)

        def fset(self, value):
            f(self, value)
    return property(fget, fset)


def _redirect(cls_name: str, args: tuple, kwargs: dict) -> tuple[tuple, dict]:
    target = os.environ.get("TSC_PYCARLA_REDIRECT")
    if cls_name != "Client" or not target:
        return args, kwargs
    host, _, port = target.rpartition(":")
    kwargs = {k: v for k, v in kwargs.items() if k not in ("host", "port")}
    return (host, int(port)) + tuple(args[2:]), kwargs


def _make_class(name: str, spec: dict, bases: tuple) -> type:
    ns: dict = {"__slots__": (), "__module__": "carla", "__qualname__": name}
    init = spec.get("init")
    if init:
        fn = _fn(init)

        def __init__(self, *args, **kwargs):
            args, kwargs = _redirect(name, args, kwargs)
            self._o = _call(fn, args, kwargs)._o
    else:
        def __init__(self, *args, **kwargs):
            raise TypeError(f"carla.{name} cannot be created from Python")
    ns["__init__"] = __init__
    for f in spec.get("fields", []):
        get = _fn(f["get"])
        sett = _fn(f["set"]) if f.get("set") else None
        ns[f["name"]] = property(
            lambda self, g=get: _wrap(g(self._o)),
            (lambda self, v, s=sett: s(self._o, _unwrap(v))) if sett else None)
    members = spec.get("members", {})
    for key, entry in members.items():
        if key.endswith(".setter"):
            continue
        if entry["kind"] == "property":
            ns[key] = _property(name, key, entry, members.get(key + ".setter"))
        elif entry["kind"] == "static":
            ns[key] = _static(name, key, entry)
        elif key == "__iter__":
            m = _method(name, "__iter__", entry)
            ns["__iter__"] = lambda self, m=m: iter(m(self))
        else:
            ns[key] = _method(name, key, entry)
    ns.update(spec.get("constants", {}))
    if "__eq__" in ns and "__hash__" not in ns:
        ns["__hash__"] = None
    return type(name, bases, ns)


def _enum(name: str, members: dict[str, int]):
    e = enum.IntEnum(name, members)
    e.__module__ = "carla"
    e.names = {k: e[k] for k in members}
    e.values = {int(v): v for v in e}
    return e


def install(g: dict) -> None:
    classes = _SPEC["classes"]
    made: dict[str, type] = {}

    def make(name: str) -> type:
        if name in made:
            return made[name]
        spec = classes[name]
        if "enum" in spec:
            made[name] = _enum(name, spec["enum"])
            return made[name]
        bases = tuple(make(b) for b in spec.get("bases", []) if b in classes) or (CarlaObject,)
        made[name] = cls = _make_class(name, spec, bases)
        box = getattr(_ext, f"B_{name}", None)
        if box is not None:
            _BOXES[box] = cls
        return cls

    for name in classes:
        g[name] = make(name)
    for name, members in _SPEC["int_enums"].items():
        g[name] = _enum(name, members)
    for alias, target in _SPEC["aliases"].items():
        if target in g:
            g[alias] = g[target]
    command = types.ModuleType("carla.command")
    for name, entry in _SPEC["functions"].items():
        if entry["fn"] is None:
            fn = _stub(name, entry.get("reason", ""))
        else:
            def fn(*a, _f=_fn(entry["fn"]), **k):
                return _call(_f, a, k)
            fn.__name__ = name
        if entry["module"] == "command":
            setattr(command, name, fn)
        else:
            g[name] = fn
    for name, value in _SPEC.get("command_constants", {}).items():
        setattr(command, name, value)
    if "Command" in g:
        command.Command = g["Command"]
    g["command"] = command
    sys.modules["carla.command"] = command
    g["CarlaObject"] = CarlaObject
