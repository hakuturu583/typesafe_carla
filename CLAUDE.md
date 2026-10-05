# typesafe_carla

Statically typed CARLA (UE5) client for Codon: Codon API → C ABI
(`native/include/typesafe_carla/ffi.h`) → C++ shim → LibCarla. Design:
`docs/design.md`. Status and milestones: `README.md`. Release process:
`docs/releasing.md`.

## Commands

```sh
TSC_BACKEND=mock uv sync                                  # dev install (+ bundled Codon via toolchain/)
cmake -S . -B build -DTSC_BACKEND=mock && cmake --build build -j
ctest --test-dir build                                    # C ABI tests
uv run pytest                                             # compile-pass/fail, runtime, launcher, architecture
uv run typesafe-codon run examples/connect.py
uv run typesafe-codon pycarla                             # build `import typesafe_carla.carla as carla` (CPython, ~15+ min)
uv run python -m tools.bindgen generate                   # after editing bindings/*.yaml
uv run python -m tools.bindgen validate --build-dir build # spec vs LibCarla/mock headers (docs/bindgen.md)

# Real LibCarla (CARLA UE5; default ref ue5-dev). Slow first build.
cmake -S . -B build-carla -DTSC_CARLA_GIT_REF=ue5-dev && cmake --build build-carla -j
# add -DPREFER_CLONE=ON where GitHub archive downloads are blocked
```

## Rules

- No CPython on the core path: no `from python import`, `pyobj` or `@python`
  in `codon/` (enforced by `tests/test_architecture.py`).
- C ABI: opaque refcounted handles, status codes + thread-local error,
  output pointers for structs, no exceptions across the boundary. Any ABI
  change bumps `TSC_ABI_VERSION_*` in both `ffi.h` and `_ffi.codon`.
- New API needs both a `tests/compile/pass` and, where misuse is possible, a
  `tests/compile/fail` case with an `# expect-error:` line. Test programs must
  *call* their functions: Codon only type-checks functions that are called.
  Strict mode (`typesafe-codon --strict`): a pass program with a `# strict`
  line must also compile in strict mode; `tests/compile/strict_fail` programs
  must compile normally and fail in strict mode with their `# expect-error:`.
- Python-API compatibility shortcuts (not statically checked): an Actor
  shortcut (`_actor_compat.codon`) takes `self: S, S: type` and starts with
  `_plain_actor_only(self, "<name>()")`, a compile error on typed subclasses
  (which inherit it), then
  `compat_shortcut("Actor.<name>", "[tsc-compat] <what>", "<instead>")` with
  literal strings (`_strict.codon`): a compile error in strict mode, else a
  warning. The launcher finds the marker in the LLVM IR to warn at compile
  time. `tests/test_architecture.py` checks this order for every shortcut.
- A C function that only checks a handle, converts arguments and calls one
  LibCarla method belongs in `bindings/*.yaml` (generated), not hand-written.
  Never edit generated code (`native/src/generated/`, marked blocks in
  `ffi.h` and `_ffi.codon`). A method that a supported LibCarla lacks gets
  `optional: {name: Class.python_name, missing_in: [<CARLA refs>]}`: the
  generated code calls it through `TSC_CALL_OPTIONAL` (TSC_ERROR where it is
  missing), and `validate` accepts it missing only on a libcarla build of a
  listed ref (never on the mock).
  Put argument validation and struct conversion in the type (`to_carla`
  overload or `check_*` helper in `internal.hpp`) so the function stays
  generatable; `self.codon` binds a value struct (e.g. `tsc_transform_t`)
  as self. What stays hand-written and why: `docs/bindgen.md`.
- A new API parameter typed `Vector3D` or `Location` is generic (no
  annotation) and goes through `geometry._vector_arg` / `_location_arg`
  (`_vector_or` / `_location_or` if optional), with a distinct literal
  `"[tsc-compat] a Location passed as a Vector3D to X"` (or the reverse);
  the helpers call `compat_shortcut`. It needs a `tests/compile/strict_fail`
  case for the compat path.
- The mock (`native/mock`) mirrors LibCarla UE5 (ue5-dev) signatures *and
  behaviour* (return values, empty results, exceptions); keep it in sync with
  any LibCarla API the shim starts using. Where 0.10.0 behaves differently,
  the shim maps both to one documented status.
- Versions: `python/typesafe_carla/__init__.py` and
  `codon/typesafe_carla/__init__.codon` must agree.

## Codon 0.19 gotchas

- Exceptions are `Static[Exception]`; no catchable hierarchy.
- `__del__` is not auto-registered; `_ffi.Handle` calls `register_finalizer`.
- Forward references in return types: define later and attach with `@extend`.
- `str.__copy__` does not copy; use `str.memcpy` into a new buffer.
- No `f"{x:.6f}"` (needs a locale); use `str(float)`.
- A comprehension over a list returned by a method called on an implicitly
  unwrapped `Optional` crashes the compiler (`build`: segfault, rc 139; `run`:
  `'<unknown type>' does not match expected type 'List[T]'`), even through a
  variable. Repro: `wp = get()` (`-> Optional[W]`), `xs = wp.ls()`,
  `[x for x in xs]`. Use `unwrap(wp)` or a plain `for` loop.
- `codon build -release` aborts (capture.cpp:618, "found multiple synthetic
  assignments for loop var") on a lambda that captures a loop variable;
  move the loop body into a function taking the variable.
- Subclasses do not upcast inside `Optional` (`Optional[Location]` is not an
  `Optional[Vector3D]`), and `isinstance` is exact
  (`isinstance(location, Vector3D)` is False).
- An argument whose type is inferred late (e.g. reached through an implicitly
  unwrapped `Optional`) can mis-bind against a parameter annotated with a base
  class, giving "'Vector3D' does not match expected type 'Location'". Use a
  generic parameter plus `_upcast_vector` / `_upcast_location`. Same-name
  methods with different signatures across a class hierarchy are dispatched
  as virtual overrides: avoid them.
- Class hierarchies two levels deep are miscompiled: with `C(B(A))`, methods
  `C` inherits from `B` read `A`'s fields at the wrong offset, and
  `super().__init__()` chains through both levels crash. Keep classes one
  level below `Actor` (`TrafficLight` and `TrafficSign` are siblings).
- `-D` definitions are visible only in the main file, and `CODON_PATH` holds
  a single directory. The strict setting reaches the library through the
  launcher-generated `_tsc_build_config` module.
