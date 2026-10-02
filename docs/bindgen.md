# Binding generation

Many C ABI functions have the same shape:
1. check the handle;
2. convert the arguments;
3. call one LibCarla method;
4. convert the result into an output pointer.

These functions are generated from a declarative spec (design sections 38–39).
Everything else stays hand-written. That includes the public Codon API: generated code is
FFI plumbing only, and users never see it.

```
bindings/*.yaml ──tools/bindgen generate──┬─ ffi.h          (marked blocks)
  (spec)                                  ├─ _ffi.codon     (marked blocks)
                                          ├─ native/src/generated/bindings.cpp
                                          └─ tests/native/test_generated.c
LibCarla headers ──libclang──> bindgen validate   (does the spec match LibCarla?)
shim sources     ──libclang──> bindgen coverage   (docs/coverage.md)
```

## The spec

`bindings/types.yaml` defines the value types. For each type it gives:
- the C type and the Codon type;
- how a value converts to LibCarla (with validation, e.g. `non_negative`);
- how a value converts back from LibCarla;
- which canonical LibCarla types it may stand for.

Struct types cross the ABI by pointer. Codon passes an `@tuple` by value as
separate scalars, which does not match the C calling convention.

Each remaining file binds one LibCarla class:

```yaml
class: carla::client::TrafficLight      # whose methods are called
prefix: traffic_light                   # C names: tsc_traffic_light_<name>
self: {type: tsc_traffic_light_t, name: light, get: light_of}  # handle check (internal.hpp)
blocks:
  traffic_light:                        # a marked block in ffi.h and _ffi.codon
    set_state: {call: SetState, args: {state: traffic_light_state}}
    set_green_time: {call: SetGreenTime, args: {t: non_negative}}
    reset_group: {call: ResetGroup}
```

A function has:
- `call`: the LibCarla method;
- `args`: name to type, in order (optional);
- `out`: the result type, or `{type, name}` (optional; its default name is `out`);
- `doc`: a comment for the header (optional).

The spec above generates:

```c
TSC_API tsc_status_t tsc_traffic_light_set_green_time(tsc_traffic_light_t *light, double t);
```
```cpp
tsc_status_t tsc_traffic_light_set_green_time(tsc_traffic_light_t *light, double t) {
  return TSC_GUARD({ light_of(light).SetGreenTime(check_non_negative(t, "t")); });
}
```
```python
from C import LIB.tsc_traffic_light_set_green_time(cobj, float) -> i32
```

## Commands

```sh
uv run python -m tools.bindgen generate            # write the generated code
uv run python -m tools.bindgen generate --check    # fail (with a diff) if it is stale
uv run python -m tools.bindgen validate --build-dir build        # spec vs headers
uv run python -m tools.bindgen coverage --build-dir build-carla -o docs/coverage.md
```

`validate` and `coverage` parse the shim sources with libclang, using the flags
recorded in the build's `compile_commands.json`, so they see exactly the headers
the shim compiles against:
- with a `mock` build, the mock's mirror of LibCarla;
- with a `libcarla` build, LibCarla at that build's CARLA ref.

`validate` checks that every spec'd method exists on the class or a public base,
that it accepts the number of arguments given, and that each argument and the
result match the spec's types. When CARLA renames a method or changes a type,
CI fails at this step instead of at link or run time.

libclang needs clang's builtin headers, which the `libclang` wheel does not
include. They are found through an installed `clang` (the CI runners have one),
through `TSC_CLANG_RESOURCE_DIR`, or through the `ziglang` package:
`uv run --with ziglang==0.13.0 python -m tools.bindgen validate ...`.

## Checks

- `tests/test_architecture.py` checks that the generated code matches the spec,
  and that no generated function is also hand-written.
- `test_generated` (ctest) checks that every generated function rejects a NULL
  handle.
- CI runs `validate` against the mock (test job) and against LibCarla `ue5-dev`
  (libcarla job). The libcarla job also writes the coverage report to its
  summary.

## Adding a binding

If the new C function is a handle check, conversions and a single LibCarla call:
1. Add it to the class's YAML file under a block. A new block needs its two
   marker lines in `ffi.h` and `_ffi.codon`; `generate` prints them.
2. Run `uv run python -m tools.bindgen generate` and
   `uv run python -m tools.bindgen validate --build-dir build`.
3. Write the Codon wrapper and the tests as for any new API (see CLAUDE.md).

Anything more involved stays hand-written in `native/src/*.cpp`. That includes
structs with many fields, lists, callbacks, and functions that create handles.
