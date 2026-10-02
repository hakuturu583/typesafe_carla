# typesafe_carla

A statically typed CARLA client for [Codon](https://github.com/exaloop/codon).
Programs written against it are type-checked by the Codon compiler before they
run, and they call LibCarla through a narrow C ABI. CPython and the `carla`
Python package are not involved.

```python
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
world = client.get_world()
vehicle = world.get_actors()[0].as_vehicle()
vehicle.apply_control(carla.VehicleControl(throttle=0.2, steer=0.0))

vehicle.apply_control(carla.Transform())
# error: 'Transform' does not match expected type 'VehicleControl'   <- at compile time
```

See [docs/design.md](docs/design.md) for the full design.

## Status

This is **Milestone 0** of the design (section 43), plus parts of Milestone 1:

| Area | Implemented |
|---|---|
| Client | `Client`, `set_timeout`, `get_timeout`, `get_world`, `load_world`, `reload_world`, `get_server_version`, `get_client_version` |
| World | `id`, `get_actors`, `get_actor` (→ `Optional[Actor]`), `get_blueprint_library`, `spawn_actor`, `try_spawn_actor` (→ `Optional[Actor]`), `tick`, `get_settings`, `apply_settings` |
| Actor | `id`, `type_id`, `is_alive`, `get/set_transform`, `get/set_location`, `get_velocity`, `set_target_velocity`, `get_acceleration`, `get_angular_velocity`, `destroy`, `is_vehicle`, `as_vehicle` (checked) |
| Vehicle | `apply_control`, `get_control`, `set_autopilot` |
| Blueprints | `BlueprintLibrary` (`find`, `filter`, indexing, iteration), `ActorBlueprint` (`id`, `has_tag`, `has_attribute`, `get_attribute`, `set_attribute`), `ActorAttribute` (typed `as_bool/as_int/as_float/as_str/as_color`) |
| Values | `Location`, `Rotation`, `Transform`, `Vector2D`, `Vector3D`, `VehicleControl`, `WorldSettings`, `Color` |
| Errors | `CarlaError`, `TimeoutError`, `ActorTypeError`, `VersionError` (plus `IndexError` for lookups by key or index) |
| Tooling | `typesafe-codon` launcher, wheel layout, `uv.lock`, CI |

Not implemented yet: sensors, batch commands, Traffic Manager, maps and
waypoints, physics control, snapshots, the `typesafe-carla-toolchain` wheel.

### Backends

The C ABI shim (`native/src`) is written once against the LibCarla API and
compiles against one of two backends, selected by `TSC_BACKEND`:

* **`libcarla`**: the real LibCarla client library (set `LIBCARLA_ROOT`).
  **Not yet compiled or tested against a real LibCarla build or CARLA server.**
  The shim follows the LibCarla 0.9.15 headers, but expect build fixes the
  first time it meets them (link line, Boost and rpclib flags).
* **`mock`**: an in-memory stand-in for LibCarla (`native/mock`) with the same
  class and method signatures, a fake "server" per `host:port`, and a toy
  vehicle model. It exists so the shim, the Codon layer and the tooling can
  be built and tested without CARLA. It is not a simulator.
* **`auto`** (default): `libcarla` if `LIBCARLA_ROOT` points at a LibCarla
  install, otherwise `mock` with a CMake warning.

`typesafe-codon info` and `typesafe_carla.backend()` report which one you
have.

## Quick start (development)

Requirements: Linux x86_64, a C++17 compiler, CMake ≥ 3.20, uv, and
Codon 0.19.x (until the toolchain wheel exists).

```sh
uv sync
uv run cmake -S . -B build              # add -DTSC_BACKEND=libcarla -DLIBCARLA_ROOT=... for CARLA
uv run cmake --build build
ctest --test-dir build                  # C ABI tests

export TYPESAFE_CODON=/path/to/codon    # not needed if Codon is in ~/.codon or on PATH
uv run typesafe-codon info
uv run typesafe-codon run examples/connect.py
uv run typesafe-codon build -release -o connect examples/connect.py && ./connect
uv run pytest                           # compile-pass/fail, runtime and launcher tests
```

`typesafe-codon` passes its arguments to `codon` after setting `CODON_PATH`
(the Codon sources), `TYPESAFE_CARLA_LIB` (the native library) and
`LD_LIBRARY_PATH`. For `build`, it also gives the executable an RPATH to the
native library, so the result runs without the launcher.

## Repository layout

```
codon/typesafe_carla/      Codon API (what users import)
  _ffi.codon               raw C declarations, POD mirrors, handle ownership
native/include/.../ffi.h   the C ABI
native/src/                C ABI implementation over LibCarla
native/mock/               in-memory LibCarla stand-in (mock backend)
python/typesafe_carla/     typesafe-codon launcher, path and toolchain discovery
tests/native/              C ABI tests (ctest)
tests/compile/pass|fail/   programs that must / must not compile
tests/unit/                Codon runtime tests against the mock backend
examples/                  example programs
```

## Differences from the CARLA Python API

Most code ports by changing `import carla` to `import typesafe_carla as carla`.
Deliberate differences, all in favour of static checking:

* **Actors must be converted before using subclass methods.**
  `world.get_actor(id).apply_control(...)` does not compile; use
  `as_vehicle()`, which raises `ActorTypeError` if the actor is not a vehicle.
* **Lookups that can miss return `Optional`.** `World.get_actor` and
  `World.try_spawn_actor` return `Optional[Actor]`.
* **`Location` is not a `Vector3D`.** A position cannot be passed where a
  velocity is expected (`set_target_velocity(actor.get_location())` fails to
  compile). Convert explicitly with `as_vector()` / `Location.from_vector()`.
* **Attribute values are typed.** `ActorAttribute.as_int()` raises when the
  attribute is not an int; `str(attribute)` gives the raw value.
* **Float precision.** Values cross into LibCarla as float32, as they do in
  the Python API, so `get_control().throttle` after setting `0.2` is
  `0.2000000029802322`.

## Codon limitations found while building this

These affect how the design's guarantees should be read:

1. **Codon only type-checks functions that are called.** A function nobody
   calls is not checked, even with full annotations, so a downstream library
   is checked where an application uses it, not on its own. Every program in
   `tests/compile` calls its functions so the tests really exercise the
   checker. The programs are compiled, never run.
2. **Exceptions do not form a catchable hierarchy.** User exceptions must
   derive `Static[Exception]`, and `except CarlaError` does not catch
   `TimeoutError`. Catch the specific classes. Each one carries the native
   status in `.status`.
3. **`__del__` is not registered automatically in Codon 0.19.3.**
   `class_alloc` passes an untyped pointer to `register_finalizer`, so
   finalizers never run. `_ffi.Handle` registers its own, which is what
   releases native handles. `tests/unit/test_ownership.codon` checks this.
4. **`-D` defines take integer values only**, so the library path can't be
   baked in at compile time. It comes from `TYPESAFE_CARLA_LIB` at run time,
   with the RPATH as the fallback.
5. Float format specifiers (`f"{x:.6f}"`) need an installed `en_US` locale in
   Codon 0.19.3, so `repr`s use plain `str(float)`.

## Compatibility matrix

| typesafe_carla | ABI | Codon | LibCarla / CARLA | Platform | Tested |
|---|---|---|---|---|---|
| 0.1.0 | 1.0 | 0.19.x | 0.9.15 (target) | Linux x86_64 | mock backend only |

## License

No license has been chosen yet.
