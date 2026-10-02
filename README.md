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

Milestones (design section 43):

| Milestone | Status |
|---|---|
| 0: proof of concept | ✅ verified against a CARLA 0.10.0 server |
| 1: usable vehicle API | ✅ verified against a CARLA 0.10.0 server |
| 2: sensors | ✅ verified against a CARLA 0.10.0 server |
| 3: distribution | ✅ release pipeline verified end to end (manylinux wheels from CI, clean-container `uv sync` → `build` → `./main` against a CARLA server); publishing to PyPI needs the one-time setup in [docs/releasing.md](docs/releasing.md) |
| 4: broader compatibility | ✅ verified against a CARLA 0.10.0 server |
| 5: binding generation | ✅ 44 C ABI functions generated from `bindings/*.yaml`, spec validated against LibCarla 0.10.0 and ue5-dev with libclang, [coverage report](docs/coverage.md) |

| Area | Implemented |
|---|---|
| Client | `Client`, `set_timeout`, `get_timeout`, `get_world`, `load_world`, `reload_world`, `get_server_version`, `get_client_version`, `apply_batch`, `apply_batch_sync`, `get_trafficmanager`, `generate_opendrive_world` (+ `OpendriveGenerationParameters`), recorder: `start_recorder`, `stop_recorder`, `show_recorder_file_info`, `show_recorder_collisions`, `show_recorder_actors_blocked`, `replay_file`, `stop_replayer`, `set_replayer_time_factor` |
| World | `id`, `get_actors`, `get_actor` (→ `Optional[Actor]`), `get_blueprint_library`, `spawn_actor`, `try_spawn_actor` (→ `Optional[Actor]`), `tick`, `wait_for_tick`, `get_snapshot`, `get_map`, `get_settings`, `apply_settings`, `get_weather` / `set_weather` / `is_weather_enabled`, `get_random_location_from_navigation` (→ `Optional`), `debug` (`DebugHelper`: `draw_point`, `draw_line`, `draw_arrow`, `draw_box`, `draw_string`) |
| Actor | `id`, `type_id`, `is_alive`, `bounding_box`, `get/set_transform`, `get/set_location`, `get_velocity`, `set_target_velocity`, `get_acceleration`, `get_angular_velocity`, `destroy`, `set_target_angular_velocity`, `add_impulse`, `add_force`, `add_angular_impulse`, `add_torque`, `set_simulate_physics`, `set_enable_gravity`; checked `as_vehicle` / `as_sensor` / `as_walker` / `as_walker_ai_controller` / `as_traffic_light` |
| Vehicle | `apply_control`, `get_control`, `set_autopilot`, `get_physics_control`, `apply_physics_control`, `set_light_state` / `get_light_state` (`VehicleLightState`), `get_speed_limit`, `get_traffic_light_state`, `is_at_traffic_light`, `get_traffic_light` (→ `Optional`) |
| Walkers | `Walker` (`apply_control(WalkerControl)`, `get_control`), `WalkerAIController` (`start`, `stop`, `go_to_location`, `set_max_speed`) |
| Traffic lights | `TrafficLight` (`get_state` / `set_state` (`TrafficLightState`), green/yellow/red times, `get_elapsed_time`, `freeze`, `is_frozen`, `get_pole_index`, `reset_group`) |
| Traffic Manager | `TrafficManager` (`set_synchronous_mode`, `set_random_device_seed`, `set_hybrid_physics_mode`, `global_percentage_speed_difference`, `set_global_distance_to_leading_vehicle`, per-vehicle `vehicle_percentage_speed_difference`, `distance_to_leading_vehicle`, `random_left/right_lanechange_percentage`, `ignore_lights/signs/vehicles/walkers_percentage`, `keep_right_rule_percentage`, `set_desired_speed`, `vehicle_lane_offset`, `auto_lane_change`, `force_lane_change`, `update_vehicle_lights`, `get_port`) |
| Weather | `WeatherParameters` (all 14 fields, `WeatherParameters.preset("ClearNoon")` for LibCarla's named presets) |
| Map | `name`, `get_spawn_points`, `get_waypoint` (→ `Optional[Waypoint]`), `generate_waypoints`, `to_opendrive`, `get_topology`, `get_crosswalks`, `get_all_landmarks`, `get_all_landmarks_of_type`; `LaneType`, `Landmark`, `Junction` (`id`, `bounding_box`, `get_waypoints`) |
| Waypoint | `id`, `transform`, `road_id`, `section_id`, `lane_id`, `s`, `is_junction`, `junction_id`, `lane_width`, `lane_type`, `next`, `previous`, `next_until_lane_end`, `previous_until_lane_start`, `get_left_lane` / `get_right_lane` / `get_junction` (→ `Optional`) |
| Snapshots | `WorldSnapshot` (`id`, `frame`, `timestamp`, `find` → `Optional`, `has_actor`, indexing, iteration), `ActorSnapshot`, `Timestamp` |
| Sensors | `Actor.as_sensor()` (checked), `Sensor.listen(callback)` (dispatched on the program's thread at `tick` / `wait_for_tick` / `carla.dispatch_sensor_callbacks()`), `Sensor.listen(queue_size)` / `stop` / `destroy` / `poll` (→ `Optional[SensorData]`) / `wait_for_data` / `has_callback` / `pending_count` / `dropped_count`; `SensorData.as_image()` / `as_lidar()` / `as_gnss()` / `as_imu()` / `as_collision()` (checked); `Image` (zero-copy `raw_data()`, `pixel`), `LidarMeasurement` (zero-copy `raw_points()`, iteration, `get_point_count`), `GnssMeasurement`, `IMUMeasurement`, `CollisionEvent` |
| Batch commands | `carla.command.SpawnActor(...).then(...)`, `FutureActor`, `DestroyActor`, `ApplyVehicleControl`, `ApplyWalkerControl`, `ApplyTransform`, `ApplyLocation`, `ApplyTargetVelocity`, `ApplyTargetAngularVelocity`, `ApplyImpulse`, `ApplyForce`, `ApplyAngularImpulse`, `ApplyTorque`, `SetAutopilot`, `SetSimulatePhysics`, `SetEnableGravity`, `SetVehicleLightState`, `SetTrafficLightState`; `CommandResponse` |
| Blueprints | `BlueprintLibrary` (`find`, `filter`, indexing, iteration), `ActorBlueprint` (`id`, `has_tag`, `has_attribute`, `get_attribute`, `set_attribute`), `ActorAttribute` (typed `as_bool/as_int/as_float/as_str/as_color`) |
| Values | `Location`, `Rotation`, `Transform`, `Vector2D`, `Vector3D`, `BoundingBox`, `VehicleControl`, `VehiclePhysicsControl`, `WheelPhysicsControl`, `WorldSettings`, `Color` |
| Errors | `CarlaError`, `TimeoutError`, `ActorTypeError`, `VersionError` (plus `IndexError` for lookups by key or index) |
| Tooling | `typesafe-codon` launcher, `typesafe-carla-toolchain` (bundled Codon), uv workspace + `uv.lock`, CI, PyPI release workflow, binding generator ([docs/bindgen.md](docs/bindgen.md)) |

Notes on Milestone 4:
- **C ABI 2.0.** `tsc_command_t` gained a field (walker speed), an incompatible change made before the first release. The Codon module checks the major version on import.
- **Weather depends on the server.** It can be disabled there: CARLA 0.10.0 (the Docker image used for testing) reports `is_weather_enabled() == False`, and `set_weather` has no effect, exactly as with the official Python API.
- **Traffic Manager.** It runs inside LibCarla in the client process. In synchronous mode, call `tm.set_synchronous_mode(True)` as well.
- **Enumerations.** `TrafficLightState`, `VehicleLightState` and `LaneType` are integer constants, as in the Python API (`VehicleLightState` values combine with `|`).

Notes on Milestone 5:
- **Generated plumbing, hand-written API.** 44 C ABI functions are generated from
  `bindings/*.yaml`: the C declarations, the C++ shim and the Codon FFI. Each one is a handle check,
  argument conversions and a single LibCarla call. The ABI is unchanged; libclang compared every
  prototype and struct size before and after the migration. See [docs/bindgen.md](docs/bindgen.md).
- **Validated against LibCarla.** `tools.bindgen validate` parses the shim with libclang
  and checks each spec'd method's existence, arity and types. It runs against the mock
  headers and LibCarla ue5-dev in CI, and was run locally against 0.10.0.
- **Coverage.** [docs/coverage.md](docs/coverage.md) lists the public methods of the main
  LibCarla client classes and whether the shim calls them (generated, hand-written or not yet).

Notes on Milestone 2:
- **No callbacks on LibCarla threads (design §15).** LibCarla's threads only fill a per-sensor queue. `listen(callback)` runs the callback on the program's own thread, at `World.tick()`, `World.wait_for_tick()`, `Client.apply_batch(_sync)(do_tick=True)` and `carla.dispatch_sensor_callbacks()`; its queue is unbounded by default. `stop()`, `destroy()` (through any handle) and batch `DestroyActor` unregister it. `listen()` / `listen(queue_size)` is polling mode: the program reads a bounded queue (64 by default) with `poll()` / `wait_for_data()`, and the oldest measurement is dropped when it is full (`dropped_count`). `queue_size=0` means unbounded in both modes. Call `stop()` (or destroy the sensor) when done.
- **Zero copy (design §16).** `Image.raw_data()` (BGRA) and `LidarMeasurement.raw_points()` point into LibCarla's buffer and stay valid while the measurement object is alive.
- `World.spawn_actor(..., attach_to=...)` accepts any actor subclass (e.g. a `Vehicle`) or an `Optional` of one, and rejects non-actors at compile time.

Notes on Milestone 1:
- **Physics control.** `VehiclePhysicsControl` is a typed subset (mass, drag, torque, rpm, gearing, center of mass, and per-wheel radius/width/mass/steer/brake/friction). `apply_physics_control` overwrites only these fields and keeps the vehicle's other settings. CARLA 0.10.0 applies changes a few frames later, and it ignores some per-wheel fields (e.g. `max_brake_torque`), exactly as the official Python API does.
- **Batch commands.** These take actor ids (`actor.id`), and every constructor returns one `Command` type, so one list can mix command kinds. `SetAutopilot` in `apply_batch_sync` also registers the vehicle with the Traffic Manager, like the Python API.

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

| typesafe_carla | ABI | Codon | Python | CARLA | Platform | Tested |
|---|---|---|---|---|---|---|
| 0.1.0 | 2.1 | 0.19.x | ≥ 3.10 (launcher only) | UE5: `ue5-dev` (default), `0.10.0` | Linux x86_64 | `0.10.0`: integration and compatibility tests pass against a CARLA 0.10.0 server. `ue5-dev`: builds, links, C ABI tests pass |

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

## Installation

See [docs/usage.md](docs/usage.md) for using typesafe_carla from your own
project. Once it is published:

```sh
uv add typesafe-carla        # or: pip install typesafe-carla
uv run typesafe-codon run main.py
```

Python 3.10 or newer (CI tests 3.10 and 3.14). Python only runs the
`typesafe-codon` launcher; your programs are compiled by Codon.
`typesafe-carla` depends on `typesafe-carla-toolchain`, which bundles a
pinned Codon. Requirements and building for another CARLA ref are in
[docs/usage.md](docs/usage.md); the release process is in
[docs/releasing.md](docs/releasing.md).

## Testing against a real CARLA server

`tests/integration/*.codon` and `tests/compatibility/` need a running CARLA
UE5 server and the `libcarla` backend built from the matching ref (a
`0.10.0` server needs `-DTSC_CARLA_GIT_REF=0.10.0`):

```sh
cmake -S . -B build-carla -DTSC_CARLA_GIT_REF=0.10.0 && cmake --build build-carla -j
export TSC_CARLA_HOST=localhost TSC_CARLA_PORT=2000 TYPESAFE_CARLA_BUILD_DIR=build-carla
uv run pytest tests/test_integration.py -s
# Same scenario through the official Python API and typesafe_carla, compared:
CARLA_PYTHON=/path/to/venv-with-carla/bin/python uv run python tests/compatibility/compare.py
```

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

## Continuing a cloud session locally

Claude Code sessions on the web can be pulled into a local terminal on any
machine. Run this from a checkout of this repository, logged in to the same
claude.ai account:

```sh
claude --teleport <session-id>    # or plain `claude --teleport` for a picker
```

Teleport fetches and checks out the session's branch (it must be pushed) and
restores the conversation. The working tree must be clean; you are prompted
to stash otherwise. It requires claude.ai login (not an API key). Project
context for new sessions is in `CLAUDE.md`.

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
tools/bindgen/             binding generator, libclang spec validation, coverage
bindings/                  binding spec (YAML) for the generated C ABI functions
native/src/generated/      generated C++ shim (do not edit)
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
* **`Location` is not a `Vector3D`.** A position cannot be passed where a
  velocity is expected (`set_target_velocity(actor.get_location())` fails to
  compile). Convert explicitly with `as_vector()` / `Location.from_vector()`.
* **Attribute values are typed.** `ActorAttribute.as_int()` raises when the
  attribute is not an int; `str(attribute)` gives the raw value.
* **Sensor callbacks run at dispatch points, not on CARLA's threads.**
  `sensor.listen(lambda data: ...)` works, but the callback receives a
  `SensorData` (convert it with `as_image()` etc.; a callback typed
  `(image: carla.Image)` does not compile) and runs on the program's own
  thread: inside `World.tick()`, `World.wait_for_tick()`,
  `Client.apply_batch_sync(..., do_tick=True)` (after the server has
  answered), `Client.apply_batch(..., do_tick=True)` (right after sending: it
  does not wait), or when the program calls
  `carla.dispatch_sensor_callbacks()` (from one thread only). A blocking
  loop that never reaches one of these never sees its callbacks run. In
  synchronous mode a measurement of frame N may reach the client just after
  `tick()` returned N; it is then delivered at the next dispatch point, so
  call `dispatch_sensor_callbacks()` (in a short wait loop if needed) when a
  frame's data is required right after its tick. An exception raised by a
  callback propagates out of the call that dispatched it, after the other
  sensors' callbacks have run. `stop()`, `destroy()` through any handle, or a
  batch `DestroyActor` unregisters the callback; stop or destroy callback
  sensors before the program exits. Callback mode
  queues without bound by default (`listen(cb, queue_size=n)` bounds it);
  `poll()` / `wait_for_data()` are for polling mode only.
* **Float precision.** Values cross into LibCarla as float32, as they do in
  the Python API, so `get_control().throttle` after setting `0.2` is
  `0.2000000029802322`.

**Not a difference: lookups that can miss return `None`, as in Python.**
`World.get_actor`, `World.try_spawn_actor`, `ActorList.find`,
`WorldSnapshot.find`, `Map.get_waypoint`, `Waypoint.get_left_lane` /
`get_right_lane` / `get_junction`, `Vehicle.get_traffic_light` and
`World.get_random_location_from_navigation` return `None` in the same cases as
the official API: an unknown actor id, an occupied spawn point, no lane at
the location or beyond the outermost lane, no junction, no traffic light, no
navigation mesh. (`Sensor.poll`, which has no Python counterpart, returns
`None` when the queue is empty.) The tests check these cases against a real
server, except the navigation-mesh case, which only the mock exercises.
`get_actor(id)` of an actor destroyed in the same episode still returns an
`Actor`: LibCarla caches actors on the client and never evicts them, and the
official API does the same. Use the world snapshot to tell whether an actor
still exists, not `is_alive`: a real server can report `True` for such an
actor, while the mock backend reports `False`.

Python-style code works without changes in most cases: `if world.get_actor(id) is None`,
`if wp:`, and using the value directly when it is known to exist
(`world.try_spawn_actor(bp, t).destroy()`, `m.get_waypoint(loc).next(2.0)`).
The exception is a chained lookup passed into *annotated* code, which needs
an explicit `unwrap()` (see [Codon limitation 5](#codon-limitations-found-while-building-this)).
The return type is `Optional[T]`, which only adds static information: the
checker knows what the value is when it is not `None`, so
`actor.set_transform(m.get_waypoint(loc))` is a compile error. Two details
differ at run time:

* Using a `None` value raises `ValueError` (`optional unpack failed: expected
  Actor, got None`) where Python raises `AttributeError` (`'NoneType' object
  has no attribute ...`).
* For an id outside the uint32 range (e.g. `-1`), `get_actor` and the `find`
  lookups return `None`; the official API raises `OverflowError`.

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
5. **`Optional[T]` is implicitly unwrapped.** Codon accepts an
   `Optional[Waypoint]` where a `Waypoint` is expected and raises at run
   time if it is `None`, so forgetting the `is None` check after
   `get_actor`, `get_left_lane` and similar calls is not a compile error.
   Codon does not narrow types after an `is not None` check either.
   Implicit unwrapping also has a gap: the result of an `Optional`-returning
   method called on an `Optional` value (`left = wp.get_left_lane()` where
   `wp = m.get_waypoint(loc)`) cannot go where a non-`Optional` type is
   *annotated* (a typed parameter, `x: Waypoint = ...`, a `-> Waypoint`
   return): Codon reports `'Waypoint' does not match expected type
   'Optional[Waypoint]'`, even after an `is not None` check. Unannotated
   code is not affected. Unwrap explicitly with Codon's `unwrap()`:
   `unwrap(wp).get_left_lane()` or `unwrap(wp.get_left_lane())`.
6. Float format specifiers (`f"{x:.6f}"`) need an installed `en_US` locale in
   Codon 0.19.3, so `repr`s use plain `str(float)`.

## License

No license has been chosen yet; one is required before the first PyPI
release. LibCarla (MIT) is linked into the native library and its license is
shipped as `_native/LICENSE.CARLA`. Codon (Apache-2.0) is redistributed by
`typesafe-carla-toolchain` with its license.
