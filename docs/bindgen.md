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

Enumerations cross as `int32_t` and convert through a checking function in
`to_carla` (`to_enum<E>({}, first, last, "what")` from `internal.hpp`, or
`to_light_state({})`), which rejects values outside the enumeration; `invalid`
is such a value.

`string_list` (output) is a `tsc_string_list_t`, filled by `string_list_assign`
and freed with `tsc_string_list_free`. Arrays cross with `c_param` as a
pointer and a count (`location_path`, `road_option_route`: `const T *{name},
size_t count`); NULL is accepted for a count of 0. Plain integer arrays use
`uint32_array` (e.g. actor ids, `World::GetActors(actor_ids)`) and
`uint64_array` (environment object ids); both convert with the
`to_vector<T>` template in `internal.hpp`, so another width needs only a
new entry in `types.yaml`.

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

`self.get` is any accessor from `native/src/internal.hpp` that validates the
handle and returns the LibCarla object. It need not be an actor: in
`obstacle_detection_event.yaml` it is `obstacle_event_of`, which takes a
sensor data handle and fails with `TSC_TYPE_ERROR` unless it holds an
`ObstacleDetectionEvent`. `self.type` is pasted into the C signature as
written, so it may be `const`-qualified (`const tsc_sensor_data_t` there).

`self` may also be a value struct passed by pointer rather than a handle
(issue #31): `transform.yaml` binds `carla::geom::Transform` with
`self: {type: const tsc_transform_t, name: transform, get: transform_of,
codon: "Ptr[CTransform]"}`. `transform_of` checks the pointer and converts the
value; `self.codon` is the self parameter's Codon type (default `cobj`).

A function has:
- `call`: the LibCarla method;
- `args`: name to type, in order (optional);
- `out`: the result type, or `{type, name}` (optional; its default name is `out`);
- `doc`: a comment for the header (optional).
- `optional`: `{name, missing_in}`, for a method that a supported LibCarla
  lacks (e.g. ue5-dev only). The call goes through `TSC_CALL_OPTIONAL` and
  raises TSC_ERROR naming `name` (e.g.
  `TrafficManager.global_large_vehicle_wide_turn`) where the method is
  missing. `validate` accepts the method missing only on a libcarla build
  whose `TSC_CARLA_GIT_REF` is in `missing_in` (e.g. `["0.10.0"]`); the mock
  mirrors ue5-dev and must have it, so a misspelt `call` still fails. Not
  allowed with `out`.

`spec.load` rejects malformed entries with a `SpecError`: unknown keys, unknown
types, an output-only type as an argument (or the reverse), a handle output,
and two C parameters with the same name (e.g. two arrays whose length is
`count`). `tests/test_bindgen.py` covers these.

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

### Handle outputs and multi-parameter inputs

Two kinds of types in `types.yaml` go beyond a single value (issue #22):

- **Handle outputs.** An output type with `handle: true` creates a new handle
  from the LibCarla result: its `from_carla` is the expression, e.g.
  `"new tsc_landmark_list(without_nulls({}))"` for a list, or
  `"new_or_null<tsc_waypoint>({})"` for an optional object (NULL means "none"). The C
  function returns it through `T **out`, and the body runs inside
  `new_handle`, so `*out` is NULL whenever the call fails:

  ```yaml
  get_waypoint_xodr:
    call: GetWaypointXODR
    args: {road_id: uint32, lane_id: int32, s: finite}
    out: waypoint_or_null
  ```
  ```cpp
  tsc_status_t tsc_map_get_waypoint_xodr(const tsc_map_t *map, uint32_t road_id, int32_t lane_id,
                                         double s, tsc_waypoint_t **out) {
    return new_handle(__func__, out, [&] {
      return new_or_null<tsc_waypoint>(
          map_of(map).GetWaypointXODR(road_id, lane_id, check_finite(s, "s")));
    });
  }
  ```
- **Inputs over several C parameters.** `c_param` is a template for the C
  parameters of an input (`{name}` is the argument name); `codon` and
  `invalid` then list one value per parameter. `string_in` is a
  (pointer, length) string:

  ```yaml
  string_in:
    c: const char *
    c_param: "const char *{name}, size_t {name}_len"
    codon: "cobj, int"
    to_carla: 'to_string({name}, {name}_len, "{name}")'
    invalid: "NULL, 1"
  ```

  `c_param` works for outputs too, with `assign` writing them. A buffer
  (two-call pattern) and an optional struct:

  ```yaml
  bounding_box_buffer:
    c_param: "tsc_bounding_box_t *{name}, size_t capacity, size_t *{name}_count"
    codon: "Ptr[CBoundingBox], int, Ptr[int]"
    assign: "copy_out({}, {out}, capacity, {out}_count)"
  optional_lane_marking:
    c_param: "int32_t *has_value, tsc_lane_marking_t *{name}"
    assign: "assign_optional({}, has_value, {out})"
  ```

  `capacity` and `has_value` are fixed parameter names (not derived from
  `{name}`), so a function can have at most one buffer or optional output:
  two would declare the same C parameter twice.

### Outputs are checked before the call

A NULL output must fail before LibCarla is called: `tsc_world_tick(w, 1.0,
NULL)` must not tick. C++17 evaluates the right side of `=` first, so the
generated code never writes `*require_ptr(out, "out") = call`:
- a plain output goes through `assign_out(out, "out", [&] { return ...; })`
  (`internal.hpp`): it checks `out`, runs the call, and zeroes `*out` if the
  call fails, so a caller never sees (or frees) stale data;
- an `assign` output first checks the C parameters listed in the type's
  `require` (default `["{out}"]`, or none with `c_param`; a buffer requires
  `["{out}_count"]`, since its `out` may be NULL to only count);
- a handle output goes through `new_handle` (`*out` is NULL on failure).

`test_generated` passes valid outputs next to the NULL handle, so the handle
check is what it tests; `test_ffi` checks the NULL-output cases.

A handle *input* (`handle: true` with `to_carla`, e.g. `vehicle`, `landmark`)
converts with `to_carla` as before. A handle type's `codon` defaults to `cobj`,
and an output's `from_carla` to a new handle of the result (`c: tsc_world_t`:
`new tsc_world({})`).

### List accessors

`bindings/lists.yaml` gives one line per list handle. Each entry generates
`size_t tsc_<list>_size` (0 for NULL or another kind of handle) and one element
getter per output type, which fails with TSC_NOT_FOUND past the end
(`list_at` in `internal.hpp`). `items` must be an lvalue (a member of the
handle): a getter's result may refer into it, and `list_at` rejects a
temporary at compile time. `items` and `what` are required:

```yaml
waypoint_list: {items: "{}->waypoints", what: waypoint list, get: waypoint_handle}
```
```cpp
tsc_status_t tsc_waypoint_list_get(const tsc_waypoint_list_t *list, size_t index,
                                   tsc_waypoint_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_waypoint(non_null(list_at(check_handle(list, "list", TSC_KIND_WAYPOINT_LIST)
                                                 ->waypoints, index, "waypoint list"), "waypoint"));
  });
}
```

They name no LibCarla method in the spec, so `validate` skips them. `coverage`
still counts the container's `size` and `at` (e.g. `ActorList::at`) as
generated: it attributes a call to the generated code when that code makes it,
directly or through a shim function template it instantiates such as
`list_at`. libclang leaves a template's dependent calls unresolved, so
`coverage` resolves `param.method()` (or `param->method()` on a raw pointer)
on each specialization's parameter types. It does not see calls on other
expressions, `->` through a smart pointer, or calls through a nested template;
none occur in the shim today, and they would show as unbound or hand-written,
never as falsely generated.

A call that differs between LibCarla versions names a `carla_compat.hpp`
helper with `via`; the function then calls `via(self, args...)`, and
`validate` accepts a real LibCarla without the method (CARLA 0.10.0). The mock
mirrors the newest LibCarla, so `validate` against the mock requires the method,
which catches a misspelt `call`:

```yaml
is_rht: {call: IsRHT, via: waypoint_is_rht, out: bool}
```

With other arguments, the object is bound first (`[&](auto &self_) { return
via(self_, args...); }(self)`), so the handle is checked before the arguments
are converted, as in a member call: C++ evaluates function arguments in no
fixed order. A missing method can also be caught at compile time: the
skeleton helpers of issue #19 (`TSC_SKELETON_QUERY`) `static_assert` that the
method exists on the mock and on a `ue5-dev` ref.

`transform_list` (output) is a `tsc_transform_list_t`, filled by
`transform_list_assign` and freed with `tsc_transform_list_free`, like
`string_list`. `uint8_buffer` is a two-call buffer of `uint8_t` (semantic
tags).

A method that some supported LibCarla simply lacks (and that has no fallback)
uses `optional` instead (see above): the generated code calls it through
`TSC_CALL_OPTIONAL`, which raises TSC_ERROR where it is missing. `via` and
`optional` exclude each other and share `validate`'s rule (`_missing_allowed`
in clang.py): a missing method is accepted only on a real LibCarla, for
`optional` only on a ref listed in `missing_in`, and never on the mock.

```yaml
set_global_large_vehicle_wide_turn:
  call: SetGlobalLargeVehicleWideTurn
  optional: {name: TrafficManager.global_large_vehicle_wide_turn, missing_in: ["0.10.0"]}
  args: {enabled: bool}
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

Validation and conversions belong in the type, not in a hand-written
function: a struct input converts and validates in its `to_carla` overload
(`vehicle_control`, `walker_control`, `opendrive_parameters`), a checked
scalar names a `check_*` helper (`positive`, `timeout`), and
an output the caller may omit is `nullable_bool` (`store_if`).

Anything more involved stays hand-written in `native/src/*.cpp` (issue #31
reviewed every remaining function):
- callbacks and queues (sensor listen/poll, World.on_tick);
- zero-copy views of measurements and physics controls;
- converters and file writers (image conversion, PNG / PLY / OpenDRIVE files);
- several LibCarla calls combined into one struct or result (waypoint and
  traffic light info, settings, telemetry, load_world_if_different, the
  Traffic Manager actions, geo projections);
- all-or-nothing validation of many values (batches, light manager setters,
  physics control);
- handle conversions (`tsc_actor_as_*`), handle lifetime, errors and versions;
- lookups whose not-found error names the argument (`tsc_world_get_actor`,
  `tsc_blueprint_library_find`, `tsc_world_snapshot_find`).
