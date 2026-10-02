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
| Tooling | `typesafe-codon` launcher, `typesafe-carla-toolchain` (bundled Codon), uv workspace + `uv.lock`, CI, PyPI release workflow |

Not implemented yet: sensors, batch commands, Traffic Manager, maps and
waypoints, physics control, snapshots.

### Supported CARLA versions

**CARLA UE5 only** (the CMake-based `ue5-dev` branch, 0.10.x and later).
UE4 (0.9.x, `ue4-dev`) is not supported.

LibCarla is built from CARLA sources as part of the build. The default is the
latest `ue5-dev`; any branch, tag or commit SHA can be selected:

| How | Example |
|---|---|
| environment variable | `CARLA_GIT_REF=0.10.0` |
| CMake | `-DTSC_CARLA_GIT_REF=<branch\|tag\|sha>` |
| pip / uv build setting | `-C cmake.define.TSC_CARLA_GIT_REF=<ref>` |
| local checkout | `CARLA_SOURCE_DIR=~/carla` or `-DTSC_CARLA_SOURCE_DIR=...` |
| other repository (fork) | `-DTSC_CARLA_GIT_REPOSITORY=https://github.com/<you>/carla.git` |

Only `CMakeLists.txt`, `CMake/` and `LibCarla/` are fetched, as a shallow,
blob-filtered, sparse checkout (a few MB, not the multi-GB repository). A
moving branch is re-fetched only on `-DTSC_CARLA_REFRESH=ON`. The ref and the
resolved commit are compiled in: `typesafe-codon info`,
`carla.libcarla_git_ref()` / `carla.libcarla_git_commit()` in Codon, and
`_native/BUILD_INFO.json` in the wheel.

| typesafe_carla | ABI | Codon | CARLA | Platform | Tested |
|---|---|---|---|---|---|
| 0.1.0 | 1.1 | 0.19.x | UE5: `ue5-dev` (default), `0.10.0` | Linux x86_64 | Builds and links against `ue5-dev` @ 1360bb9a; C ABI tests pass. Not yet run against a CARLA server |

### Backends

`TSC_BACKEND` selects the implementation behind the C ABI. The shim in
`native/src` is shared by both:

* **`libcarla`** (default): LibCarla built from CARLA UE5 sources as above,
  linked statically into `libtypesafe_carla_ffi.so`. The resulting library
  depends only on libstdc++/libc and exports only the `tsc_*` functions.
* **`mock`**: an in-memory stand-in for LibCarla (`native/mock`) with the same
  class and method signatures, a fake "server" per `host:port`, and a toy
  vehicle model. It is used for the compile and runtime test suites and for
  development without CARLA. It is not a simulator.

`typesafe-codon info` and `typesafe_carla.backend()` report which one you
have.

## Installation (once published)

```sh
uv add typesafe-carla        # or: pip install typesafe-carla
uv run typesafe-codon run main.py
```

`typesafe-carla` depends on `typesafe-carla-toolchain`, which bundles a
pinned Codon, so no separate Codon install and no `CODON_PATH` setup is
needed. To use a different CARLA ref than the published wheel's, build from
the sdist: `CARLA_GIT_REF=<ref> pip install --no-binary typesafe-carla
typesafe-carla`. See [docs/releasing.md](docs/releasing.md) for the release
process.

## Quick start (development)

Requirements: Linux x86_64, a C++20 compiler, git, CMake ≥ 3.27.2, and uv.
Codon is installed by `uv sync` from the `toolchain/` workspace member.

```sh
TSC_BACKEND=mock uv sync                # fast: editable install with the mock backend
uv run typesafe-codon info

# Mock backend: everything the test suites need.
cmake -S . -B build -DTSC_BACKEND=mock && cmake --build build -j
ctest --test-dir build                  # C ABI tests
uv run pytest                           # compile-pass/fail, runtime and launcher tests
uv run typesafe-codon run examples/connect.py

# Real backend: LibCarla from CARLA ue5-dev (first build fetches and compiles
# LibCarla and its dependencies; takes a while).
cmake -S . -B build-carla -DTSC_CARLA_GIT_REF=ue5-dev && cmake --build build-carla -j
TYPESAFE_CARLA_BUILD_DIR=build-carla uv run typesafe-codon run examples/connect.py
```

If GitHub archive downloads are blocked by your network but git works, add
`-DPREFER_CLONE=ON`; CARLA then clones its dependencies instead.

`typesafe-codon` passes its arguments to `codon` after setting `CODON_PATH`
(the Codon sources), `TYPESAFE_CARLA_LIB` (the native library) and
`LD_LIBRARY_PATH`. For `build`, it also gives the executable an RPATH to the
native library, so the result runs without the launcher.

## Repository layout

```
cmake/FetchCarla.cmake     fetches CARLA UE5 sources at a ref (default ue5-dev)
codon/typesafe_carla/      Codon API (what users import)
  _ffi.codon               raw C declarations, POD mirrors, handle ownership
native/include/.../ffi.h   the C ABI
native/src/                C ABI implementation over LibCarla
native/mock/               in-memory LibCarla stand-in (mock backend)
python/typesafe_carla/     typesafe-codon launcher, path and toolchain discovery
toolchain/                 typesafe-carla-toolchain: pinned Codon as a wheel
tools/check_wheel.py       release checks on a built wheel
.github/workflows/         CI (mock + LibCarla builds) and PyPI release
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

## License

No license has been chosen yet; one is required before the first PyPI
release. LibCarla (MIT) is linked into the native library and its license is
shipped as `_native/LICENSE.CARLA`. Codon (Apache-2.0) is redistributed by
`typesafe-carla-toolchain` with its license.
