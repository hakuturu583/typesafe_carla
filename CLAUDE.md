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
  `ffi.h` and `_ffi.codon`).
- The mock (`native/mock`) mirrors LibCarla UE5 signatures; keep it in sync
  with any LibCarla API the shim starts using.
- Versions: `python/typesafe_carla/__init__.py` and
  `codon/typesafe_carla/__init__.codon` must agree.

## Codon 0.19 gotchas

- Exceptions are `Static[Exception]`; no catchable hierarchy.
- `__del__` is not auto-registered; `_ffi.Handle` calls `register_finalizer`.
- Forward references in return types: define later and attach with `@extend`.
- `str.__copy__` does not copy; use `str.memcpy` into a new buffer.
- No `f"{x:.6f}"` (needs a locale); use `str(float)`.
- `codon build -release` aborts (capture.cpp:618, "found multiple synthetic
  assignments for loop var") on a lambda that captures a loop variable;
  move the loop body into a function taking the variable.
