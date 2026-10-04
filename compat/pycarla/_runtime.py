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
import traceback
import types
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_SPEC = json.loads((_HERE / "_spec.json").read_text())
if not os.environ.get("TYPESAFE_CARLA_LIB") and _SPEC.get("native_library"):
    os.environ["TYPESAFE_CARLA_LIB"] = _SPEC["native_library"]

from . import _carla as _ext  # noqa: E402  (needs TYPESAFE_CARLA_LIB)

_BOXES: dict[type, type] = {}
_MEMORYVIEWS = frozenset(_SPEC.get("memoryviews", ()))


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
        if cls.__name__ in _MEMORYVIEWS:  # e.g. raw_data: a memoryview in CARLA's API
            return memoryview(bytes(obj.tolist()))
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
        return list(memoryview(v).cast("B"))  # a buffer's bytes, as CARLA's API reads it
    if isinstance(v, dict):
        return {k: _unwrap(x) for k, x in v.items()}
    if callable(v) and not isinstance(v, type):
        return _callback(v)
    return v


def _callback(fn):
    """A Python callable as the extension calls it (a sensor's listen, ...).
    As in CARLA's Python API, an exception in it is printed and the callback
    returns None: it must not escape into typesafe_carla's dispatch (and out of
    world.tick())."""
    def callback(*args):
        try:
            return _unwrap(fn(*(_wrap(x) for x in args)))
        except Exception:
            traceback.print_exc()
            return None
    return callback


_RENAMED = _SPEC.get("renamed", {})  # keyword -> the exported parameter's name


def _call(fn, args, kwargs):
    try:
        return _wrap(fn(*(_unwrap(a) for a in args),
                        **{_RENAMED.get(k, k): _unwrap(x) for k, x in kwargs.items()}))
    except BaseException as e:
        # typesafe_carla's errors (CarlaError, TimeoutError, ...) have no
        # CPython type and arrive as plain BaseException; CARLA's Python API
        # raises RuntimeError.
        if type(e) is BaseException:
            raise RuntimeError(f"{e} [{_describe(fn, args, kwargs)}]") from None
        raise


def _describe(fn, args, kwargs) -> str:
    """`carla.Client.load_world('Town10HD_Opt')`: the call an error came from."""
    name = getattr(fn, "name", "?")
    cls, _, member = name.partition("__")
    member = {"new": "__init__"}.get(member, member)
    shown = [repr(a) if isinstance(a, (str, int, float, bool)) or a is None else type(a).__name__
             for a in args[1:]] if member != "__init__" else [repr(a) for a in args]
    shown += [f"{k}={v!r}" for k, v in kwargs.items() if isinstance(v, (str, int, float, bool))]
    return f"carla.{cls}.{member}({', '.join(shown)})"


def _targets(targets: dict, args: tuple, kwargs: dict) -> tuple:
    """A command's target (`actor_id` / `actor`, `parent_id` / `parent`) as
    the wrapper takes it: by `<x>_id`, an Actor replaced by its id."""
    args, kwargs = list(args), dict(kwargs)
    for alias, (name, pos) in targets.items():
        if alias in kwargs:
            if name in kwargs or len(args) > pos:
                raise TypeError(f"pass either {name} or {alias}, not both")
            kwargs[name] = kwargs.pop(alias)
        if name in kwargs:
            kwargs[name] = _actor_id(kwargs[name])
        elif len(args) > pos:
            args[pos] = _actor_id(args[pos])
    return tuple(args), kwargs


_ACTOR = None  # carla.Actor, once the package is built


def _actor_id(v):
    return v.id if _ACTOR is not None and isinstance(v, _ACTOR) else v


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
        # Python would make the class unhashable; CARLA's (Boost.Python)
        # classes stay hashable by identity.
        ns["__hash__"] = object.__hash__
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
        elif entry.get("targets"):
            def fn(*a, _f=_fn(entry["fn"]), _t=entry["targets"], **k):
                return _call(_f, *_targets(_t, a, k))
            fn.__name__ = name
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
    global _ACTOR
    _ACTOR = g.get("Actor")
    _setup_dispatcher(g)


# --- Issue #89: sensor callbacks on a background thread -----------------------
#
# As in CARLA's Python API, callbacks run as soon as their data arrives. Per
# docs/design.md "External dispatchers" (#86): dispatch points off and unlocked
# (the GIL serializes calls; a lock held across a GIL-releasing callback would
# deadlock), and a daemon thread runs dispatch_callbacks() whenever the native
# queue signal changes, waiting for it through ctypes without the GIL.
#
# The thread starts at the first callback registration (listen,
# listen_to_gbuffer, on_tick), so importing carla stays single-threaded, and is
# restarted in a forked child. If it cannot register with Codon's collector,
# callbacks stay at typesafe_carla's dispatch points (World.tick(), ...).

_DISPATCH_WAIT = 1.0  # seconds; only bounds a missed wake-up
_REGISTERING = ("listen", "listen_to_gbuffer", "on_tick")
_hooks = None       # (g, count, wait, notify), once install() found #86's hooks
_dispatcher = None  # the running dispatcher: (pid, thread, stopping event)
_failed = False     # the thread could not attach: dispatch points stay on


def _setup_dispatcher(g: dict) -> None:
    """Prepares the dispatcher (started by _ensure_dispatcher), and makes the
    registering methods start it."""
    global _hooks
    import ctypes

    names = ("set_auto_dispatch", "auto_dispatch", "dispatch_callbacks",
             "attach_current_thread", "detach_current_thread")
    if any(_SPEC["functions"].get(n, {}).get("fn") is None for n in names):
        return  # a typesafe_carla without #86's hooks: callbacks stay at the dispatch points
    try:
        lib = ctypes.CDLL(os.environ.get("TYPESAFE_CARLA_LIB", "libtypesafe_carla_ffi.so"))
        count, wait, notify = lib.tsc_queue_signal_count, lib.tsc_queue_signal_wait, lib.tsc_queue_signal_notify
    except (OSError, AttributeError):
        return  # `import carla` must not fail over this: dispatch points stay on
    count.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
    wait.argtypes = [ctypes.c_uint64, ctypes.c_double, ctypes.POINTER(ctypes.c_uint64)]
    notify.argtypes = []
    count.restype = wait.restype = notify.restype = ctypes.c_int
    _hooks = (g, count, wait, notify)
    for cls in {v for v in g.values() if isinstance(v, type) and issubclass(v, CarlaObject)}:
        for name in _REGISTERING:
            if name in cls.__dict__:
                setattr(cls, name, _starting(cls.__dict__[name]))
    os.register_at_fork(before=_before_fork, after_in_parent=_after_fork,
                        after_in_child=_after_fork)


def _starting(method):
    def registering(*args, **kwargs):
        _ensure_dispatcher()
        return method(*args, **kwargs)
    registering.__name__ = registering.__qualname__ = method.__name__
    return registering


def _ensure_dispatcher() -> None:
    """Starts the dispatcher thread, unless it runs, failed, or another copy of
    this package dispatches (the dispatch points are already off)."""
    global _dispatcher, _failed
    import ctypes
    import threading
    import time

    if _hooks is None or _failed:
        return
    if _dispatcher is not None and _dispatcher[0] == os.getpid() and _dispatcher[1].is_alive():
        return
    g, count, wait, notify = _hooks
    if _dispatcher is None and not g["auto_dispatch"]():
        return  # another dispatcher (a second copy of the package) delivers
    stopping, attached, go = threading.Event(), threading.Event(), threading.Event()
    ok = []

    def loop() -> None:
        # Codon's collector must know the thread before it runs Codon code.
        try:
            ok.append(bool(g["attach_current_thread"]()))
        except Exception:
            ok.append(False)
        attached.set()
        if not ok[0]:
            return
        try:
            go.wait()
            seen, out = ctypes.c_uint64(), ctypes.c_uint64()
            while not stopping.is_set():
                if count(ctypes.byref(seen)) != 0:  # the signal failed: poll instead
                    time.sleep(_DISPATCH_WAIT)
                    seen.value = 0
                try:
                    g["dispatch_callbacks"]()
                except Exception:  # a callback's own errors are printed by _callback
                    traceback.print_exc()
                if wait(seen.value, _DISPATCH_WAIT, ctypes.byref(out)) != 0:
                    time.sleep(_DISPATCH_WAIT)
        finally:
            g["detach_current_thread"]()

    thread = threading.Thread(target=loop, name="carla-callbacks", daemon=True)
    thread.start()
    attached.wait()
    if not ok[0]:
        _failed = True
        g["set_auto_dispatch"](True)
        print("carla: the callback thread could not register with Codon's garbage collector; "
              "callbacks run at World.tick() and the other dispatch points", file=sys.stderr)
        return
    g["set_auto_dispatch"](False, False)  # enabled=False, lock=False (see above)
    first = _dispatcher is None
    _dispatcher = (os.getpid(), thread, stopping)
    go.set()
    if first:
        import atexit
        atexit.register(_stop_dispatcher)


def _stop_dispatcher(why: str = "the program ended") -> bool:
    """Stops the dispatcher thread (it detaches from Codon's collector); waits
    at most 2 s for a callback in progress, as typesafe_carla's own thread.
    Returns whether one was running."""
    if _dispatcher is None or _dispatcher[0] != os.getpid() or not _dispatcher[1].is_alive():
        return False
    _, thread, stopping = _dispatcher
    stopping.set()
    _hooks[3]()  # notify: wake the wait
    thread.join(2.0)
    if thread.is_alive():
        print(f"carla: a sensor callback was still running 2 s after {why}", file=sys.stderr)
    return True


_restart_after_fork = False


def _before_fork() -> None:
    """The thread is stopped across a fork: Codon's collector would otherwise
    keep it registered in the child, where it does not exist, and the child's
    next collection would wait for it forever."""
    global _restart_after_fork
    _restart_after_fork = _stop_dispatcher("fork() was called")


def _after_fork() -> None:
    """Parent and child: a new thread, if one ran before the fork (the
    listeners are still registered)."""
    global _dispatcher, _restart_after_fork
    if not _restart_after_fork:
        return
    _restart_after_fork = False
    _dispatcher = (-1,) + _dispatcher[1:]  # stopped: _ensure_dispatcher starts another
    _ensure_dispatcher()
    if _dispatcher[0] != os.getpid():
        _hooks[0]["set_auto_dispatch"](True)  # no thread: the dispatch points deliver
