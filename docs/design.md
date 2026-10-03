# typesafe_carla Design

> Implementation status and deviations found while building it are tracked
> in the [README](../README.md).

## 1. Overview

typesafe_carla is a statically typed CARLA client library designed primarily for Codon.

The goal is to provide an API that is as close as practical to the existing CARLA Python API while allowing user applications to be compiled and type-checked by Codon before execution.

The core execution path must not depend on the CARLA Python package.

```
Codon application
        │
        │ static type checking
        ▼
typesafe_carla
        │
        │ typed Codon API
        ▼
typesafe_carla C ABI
        │
        │ extern "C"
        ▼
LibCarla C++
        │
        │ CARLA RPC
        ▼
CARLA Server
```

The primary objective is therefore not merely to add Python type hints to CARLA, but to move the CARLA client API into a statically checked native execution path.

## 2. Goals

### Primary goals

1. Detect CARLA API misuse at compile time wherever possible.
2. Preserve a Python-like CARLA programming experience.
3. Avoid pyobj and CPython on the normal execution path.
4. Call LibCarla directly.
5. Allow downstream applications and libraries to be compiled by Codon.
6. Allow installation and dependency resolution through uv.
7. Make the environment reproducible through uv.lock.
8. Minimize runtime overhead between Codon and LibCarla.
9. Provide deterministic ownership and error-handling semantics across the FFI boundary.
10. Maintain a clear compatibility matrix between:
    * typesafe_carla
    * Codon
    * LibCarla
    * CARLA Server

## 3. Non-goals

The first version does not aim for:

* 100% CARLA Python API compatibility
* arbitrary CPython library compatibility
* transparent wrapping of `import carla`
* runtime monkey patching
* dynamically typed CARLA objects
* support for every CARLA version simultaneously
* Windows native support
* reproducing Python callback semantics exactly where they conflict with safe FFI design

Unsupported APIs should initially be absent rather than silently routed through Python.

The core library must not contain:

```python
from python import carla
```

as an implementation fallback.

This guarantees that the main API remains inside the Codon/native type system.

## 4. Design principles

### 4.1 Static types first

Public API objects should have explicit native types.

```python
from typesafe_carla import Client, Vehicle, Transform
client = Client("localhost", 2000)
world = client.get_world()
vehicle: Vehicle = ...
transform: Transform = vehicle.get_transform()
vehicle.set_transform(transform)
```

Incorrect code such as:

```python
vehicle.set_transform("hello")
```

must fail during Codon compilation.

Likewise:

```python
vehicle.apply_control(Transform())
```

must fail because `Vehicle.apply_control()` expects `VehicleControl`.

### 4.2 Preserve CARLA naming

Where possible, names should match the official CARLA Python API.

```
carla.Client         → typesafe_carla.Client
carla.World          → typesafe_carla.World
carla.Actor          → typesafe_carla.Actor
carla.Vehicle        → typesafe_carla.Vehicle
carla.Transform      → typesafe_carla.Transform
carla.Location       → typesafe_carla.Location
carla.Rotation       → typesafe_carla.Rotation
carla.VehicleControl → typesafe_carla.VehicleControl
```

This minimizes migration cost.

The preferred migration should look approximately like:

```diff
- import carla
+ import typesafe_carla as carla
```

followed by fixing places where the original Python program relied on dynamic behavior.

## 5. Architecture

The library is divided into four layers.

```
┌──────────────────────────────────────┐
│ User application                     │
│   planner, scenario runner,          │
│   traffic agents, controllers        │
└──────────────────────────────────────┘
                    │
                    ▼
┌──────────────────────────────────────┐
│ typesafe_carla Codon API             │
│   Client, World, Actor, Vehicle,     │
│   Sensor, Transform, Location,       │
│   VehicleControl, ...                │
└──────────────────────────────────────┘
                    │
                    ▼
┌──────────────────────────────────────┐
│ typesafe_carla FFI                   │
│   C ABI, opaque handles,             │
│   POD structures, status/error API   │
└──────────────────────────────────────┘
                    │
                    ▼
┌──────────────────────────────────────┐
│ LibCarla                             │
│   carla::client::Client, World,      │
│   Actor, Vehicle, ...                │
└──────────────────────────────────────┘
                    │
                    ▼
              CARLA Server
```

Codon currently supports calling C/C++ functionality through a typed C interface using `from C import`, with C++ functions exposed using `extern "C"`. Direct general-purpose C++ interoperability is still not the interface to rely on here, so typesafe_carla uses a narrow C ABI over LibCarla.

## 6. Why a C ABI shim

LibCarla is a C++ API.

Directly exposing C++ classes across the Codon boundary would introduce:

* C++ ABI dependence
* templates
* STL types
* `std::shared_ptr`
* exceptions
* compiler ABI compatibility issues
* object lifetime complexity

Instead:

```
Codon
  ↓
stable C ABI
  ↓
C++ shim
  ↓
LibCarla
```

The C shim owns all C++ implementation details.

```c
extern "C" {
tsc_client_t* tsc_client_create(const char* host, uint16_t port);
void tsc_client_destroy(tsc_client_t* client);
tsc_status_t tsc_client_get_world(tsc_client_t* client, tsc_world_t** out_world);
}
```

Codon sees only typed C functions.

```python
from C import LIB.tsc_client_create(cobj, int) -> cobj
```

## 7. FFI design

### 7.1 Opaque handles

Complex LibCarla objects must never be exposed directly.

```c
typedef struct tsc_client tsc_client_t;
typedef struct tsc_world tsc_world_t;
typedef struct tsc_actor tsc_actor_t;
typedef struct tsc_vehicle tsc_vehicle_t;
typedef struct tsc_sensor tsc_sensor_t;
```

Internally:

```cpp
struct tsc_vehicle {
    std::shared_ptr<carla::client::Vehicle> ptr;
};
```

Codon only holds `Ptr[tsc_vehicle]` or an equivalent opaque pointer.

## 8. Value types

Small immutable/value-like CARLA types should be represented as Codon-native classes.

```python
class Vector3D:
    x: float
    y: float
    z: float
class Location(Vector3D):  # as in the Python API (issue #10)
    ...
class Rotation:
    pitch: float
    yaw: float
    roll: float
class Transform:
    location: Location
    rotation: Rotation
class VehicleControl:
    throttle: float
    steer: float
    brake: float
    hand_brake: bool
    reverse: bool
    manual_gear_shift: bool
    gear: int
```

Equivalent C representations:

```c
typedef struct { double x; double y; double z; } tsc_location_t;
typedef struct { double pitch; double yaw; double roll; } tsc_rotation_t;
typedef struct { tsc_location_t location; tsc_rotation_t rotation; } tsc_transform_t;
```

However, FFI functions should prefer output pointers:

```c
tsc_status_t tsc_actor_get_transform(tsc_actor_t* actor, tsc_transform_t* out);
```

rather than:

```c
tsc_transform_t tsc_actor_get_transform(...);
```

Codon's C interop documentation explicitly recommends care with returned C structures because C ABI struct-return conventions can differ; output pointers avoid that issue.

## 9. Public API example

Desired API:

```python
import typesafe_carla as carla
client = carla.Client("localhost", 2000)
client.set_timeout(10.0)
world = client.get_world()
blueprints = world.get_blueprint_library()
bp = blueprints.find("vehicle.tesla.model3")
spawn = carla.Transform(
    carla.Location(10.0, 20.0, 0.5),
    carla.Rotation(0.0, 90.0, 0.0),
)
actor = world.spawn_actor(bp, spawn)
vehicle = actor.as_vehicle()
control = carla.VehicleControl(
    throttle=0.5,
    steer=0.1,
    brake=0.0,
)
vehicle.apply_control(control)
```

Incorrect code:

```python
vehicle.apply_control(spawn)
```

must result in a compile-time type error.

## 10. Actor hierarchy

Codon's type model should reflect CARLA's conceptual actor hierarchy.

```
Actor
├── Vehicle
├── Walker
├── TrafficLight
├── Sensor
│   ├── CameraSensor
│   ├── LidarSensor
│   ├── GnssSensor
│   └── ImuSensor
└── ...
```

Public API:

```python
actor: Actor
if actor.is_vehicle():
    vehicle = actor.as_vehicle()
```

Prefer explicit checked conversion over unchecked casting.

```python
vehicle: Vehicle = actor.as_vehicle()
```

If the actor is not a vehicle, `ActorTypeError` should be raised.

For compatibility with Python-API code, `Actor` also has the subclass methods
(`world.get_actor(i).apply_control(...)`): each converts with the matching
`as_*()` at run time and delegates to the subclass, so argument types stay
checked at compile time but the actor kind does not. They warn when used and
are compile errors in strict mode (`typesafe-codon --strict`).

## 11. Ownership model

All handle-backed public objects own a reference to an FFI handle.

```
Vehicle
  ↓
tsc_vehicle_t
  ↓
shared_ptr<carla::client::Vehicle>
```

Rules:

1. Each exported handle has a corresponding release function.
2. C++ exceptions never cross the C ABI.
3. Copying a Codon wrapper must either:
    * increment the native handle reference count, or
    * use shared internal handle ownership.
4. Destruction must be idempotent where practical.
5. Null handles are invalid public objects.

Possible internal C API:

```c
void tsc_handle_retain(tsc_handle_t*);
void tsc_handle_release(tsc_handle_t*);
```

A generic reference-counted handle header is preferred over implementing independent lifetime management for every object.

## 12. Error handling

No C++ exception may cross the FFI boundary.

All native functions return:

```c
typedef enum {
    TSC_OK = 0,
    TSC_ERROR = 1,
    TSC_INVALID_ARGUMENT = 2,
    TSC_TIMEOUT = 3,
    TSC_NOT_FOUND = 4,
    TSC_TYPE_ERROR = 5,
    TSC_VERSION_ERROR = 6
} tsc_status_t;
```

Example:

```c
tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t* vehicle,
                                       const tsc_vehicle_control_t* control);
```

Errors are stored in thread-local state:

```c
const char* tsc_last_error_message();
```

The Codon wrapper converts these to typed exceptions:

```python
class CarlaError(Exception): pass
class TimeoutError(CarlaError): pass
class ActorTypeError(CarlaError): pass
class VersionError(CarlaError): pass
```

Internal helper:

```python
def _check(status: int):
    if status == TSC_OK:
        return
    if status == TSC_TIMEOUT:
        raise TimeoutError(_last_error())
    raise CarlaError(_last_error())
```

## 13. String handling

Do not expose `std::string` directly. Use UTF-8 buffers.

For input:

```c
tsc_status_t tsc_blueprint_library_find(tsc_blueprint_library_t* library,
                                        const char* id, size_t id_len,
                                        tsc_blueprint_t** out);
```

For output, prefer owned buffer APIs:

```c
tsc_status_t tsc_actor_get_type_id(tsc_actor_t* actor, tsc_string_t* out);
void tsc_string_free(tsc_string_t* string);

typedef struct {
    char* data;
    size_t size;
} tsc_string_t;
```

This keeps allocation ownership explicit.

## 14. Containers

Do not expose STL containers.

For vectors of actors:

```c
size_t tsc_actor_list_size(const tsc_actor_list_t*);
tsc_status_t tsc_actor_list_get(const tsc_actor_list_t*, size_t index, tsc_actor_t** out);
```

Codon wrapper:

```python
class ActorList:
    def __len__(self) -> int: ...
    def __getitem__(self, index: int) -> Actor: ...
    def __iter__(self): ...
```

Eventually frequently used result lists can be copied into native Codon arrays to eliminate repeated FFI calls.

## 15. Sensors

Sensor callbacks are one of the most difficult ownership/threading boundaries.

The MVP should not call arbitrary Codon functions directly from LibCarla worker threads.

Instead:

```
CARLA sensor callback
        │
        ▼
C++ callback
        │
        ▼
thread-safe native queue
        │
        ▼
Codon Sensor.poll()
```

Example:

```python
camera = actor.as_camera_sensor()
while True:
    image = camera.wait_for_data()
    process(image)
```

API:

```python
data = sensor.poll()
if data is not None:
    ...
```

or:

```python
data = sensor.wait_for_data(timeout=1.0)
```

This prevents C++ worker threads from unexpectedly entering the Codon runtime.

### Callbacks through a controlled dispatcher

`sensor.listen(callback)` keeps that guarantee. The callback does not go to
LibCarla: LibCarla's threads still only fill the sensor's queue, and the
callback runs on the program's thread when the program reaches a dispatch
point:

```
CARLA sensor callback ──▶ native queue (unbounded by default)
                                 │
World.tick() / World.wait_for_tick() / Client.apply_batch(_sync)(do_tick=True)
carla.dispatch_sensor_callbacks()
                                 │
                                 ▼
                    callback(data) on the program's thread
```

```python
frames = List[int]()
camera.listen(lambda data: frames.append(data.frame))
world.tick()                          # callbacks for the data queued so far
carla.dispatch_sensor_callbacks()     # e.g. in an asynchronous main loop
```

- **Registry.** A Codon-side, process-wide list of the sensors in callback
  mode, in registration order, keyed by actor id (`_callbacks.codon`, which
  `actor.codon` imports without depending on `sensor.codon`). A World handle
  does not know its sensors, so every dispatch point drains every registered
  sensor. The registry holds the listening sensor handle, so the stream lives
  on without a reference in user code (as in the Python API) until it is
  unregistered. There is one callback per sensor actor: listening through
  another handle of the same actor replaces it. The registry is not
  thread-safe; register, stop and dispatch from one thread.
- **Order.** Per sensor, measurements are delivered in arrival order. Sensors
  are visited in registration order. One dispatch delivers only what was
  queued when the sensor's turn began (`tsc_sensor_pending_count`), so it
  returns even when a sensor produces data faster than its callback runs.
- **When.** `World.tick()`, `World.wait_for_tick()` and
  `Client.apply_batch_sync(..., do_tick=True)` dispatch after the server has
  answered; `Client.apply_batch(..., do_tick=True)` is fire-and-forget and
  dispatches right after sending, so it only delivers what had already
  arrived. In
  synchronous mode a measurement of frame N can reach the client just after
  `tick()` returned N; it is delivered at the next dispatch point, or by an
  explicit `dispatch_sensor_callbacks()`.
- **Queue.** Callback mode uses an unbounded queue by default (capacity 0 at
  the C ABI) so no measurement is lost; `listen(cb, queue_size=n)` bounds it
  and drops the oldest. Polling mode keeps its bounded default (64).
- **Errors.** If a callback raises, the pass still visits every other
  sensor (so one failing callback cannot starve the rest or let their
  unbounded queues grow), then the first exception propagates to the caller
  of the dispatching call (after the simulation has advanced, for `tick()`);
  later exceptions of the same pass are dropped. The raising sensor's
  undelivered measurements stay queued. A dispatch started from inside
  a callback (e.g. a callback that ticks) does nothing, so callbacks never
  nest.
- **Lifetime.** `stop()` or `destroy()` through any handle of the sensor
  actor (a `Sensor`, an `Actor`, a `world.get_actor(id)` handle), and a batch
  `DestroyActor` of its id (`apply_batch_sync`: when its response succeeds;
  `apply_batch`: always, as there is no response), unregister the callback,
  stop its stream and discard what it has not received; later measurements
  are never delivered. Native calls come first, so a failing `stop()` or
  `destroy()` leaves the callback registered and its queue drained. If
  `listen()` fails in LibCarla, the previous callback is gone and none is
  registered. Callbacks still registered at program exit are not released
  explicitly (Codon has no `atexit`); stop or destroy sensors before exiting,
  as with polling sensors.
- **Exclusive modes.** `poll()` / `wait_for_data()` raise `CarlaError` on a
  sensor in callback mode, so the two consumers never compete for the queue.

**World tick callbacks** (`World.on_tick`, issue #21) use the same registry
and dispatcher. LibCarla's `World::OnTick` callback only queues the
`WorldSnapshot` in a native tick listener (`tsc_world_on_tick`); the Codon
callback receives it at the same dispatch points, in registration order
together with the sensor callbacks (`carla.dispatch_callbacks()`;
`dispatch_sensor_callbacks()` is the earlier name). Tick callbacks are keyed
by (client, LibCarla callback id): LibCarla keeps them per Simulator (one
per Client), across `load_world`, so any World of the client can remove
them; `remove_on_tick(id)` removes the LibCarla registration and drops the
undelivered snapshots. LibCarla's episode publishes a tick's state before it
runs the OnTick callbacks, so `World.tick()` / `wait_for_tick()` could return
before the frame is queued. They therefore wait, within their timeout, until
each of the client's tick listeners has received the returned frame
(`tsc_tick_listener_wait_for_frame`, a frame counter and a condition
variable); a timeout there only delays delivery to a later dispatch point.

The callback type is `Callable[[SensorData], None]`. Functions, bound methods,
lambdas and closures are all accepted (Codon 0.19 cannot convert a capturing
closure to a `Callable` directly, so `listen` wraps it in a generic adapter
whose bound method is stored). A callback with the wrong signature fails to
compile.

## 16. Zero-copy sensor data

Large sensor buffers should not be copied unnecessarily.

```
LibCarla sensor buffer
        ↓
shared native buffer
        ↓
SensorData handle
        ↓
Ptr[u8] + size
```

Possible API:

```python
image = camera.wait_for_data()
data: Ptr[u8] = image.raw_data()
size: int = image.raw_size()
```

For structured data, `lidar.points()` can return a typed view over native memory when alignment and lifetime guarantees are satisfied.

The associated SensorData object must remain alive while the view is used.

Implemented views (issue #24 added the last four): `Image` (BGRA bytes),
`LidarMeasurement` ({x, y, z, intensity} floats), `SemanticLidarMeasurement`
({x, y, z, cos_inc_angle} floats + {object_idx, object_tag} uint32),
`RadarMeasurement` ({velocity, azimuth, altitude, depth} floats),
`DVSEventArray` (packed 13-byte events, decoded on access) and
`OpticalFlowImage` ({x, y} floats). In Codon they share one generic base,
`_ArrayMeasurement[T]`: `len`, indexing and iteration convert one element at a
time into the Python API's element class; `raw_data()` / `raw_size()` expose
the bytes. `Image.convert` changes the buffer in place, as in the Python API.

The V2X events of issue #42 (`CAMEvent`, `CustomV2XEvent`, LibCarla ue5-dev
only) are not views: LibCarla's `CAMData` is a large struct with
implementation-defined layout (`long`, `bool` flags, `std::array`s), so each
message is copied on access into flat C structs (`tsc_cam_message_t`, with
the two variable-length ITS lists behind their own two-call getters) and then
into typed Codon classes.

## 17. Threading

Rules:

* LibCarla may use its own threads.
* FFI state must be thread-safe.
* `tsc_last_error` must be thread-local.
* sensor queues must support producer/consumer access.
* no global Python interpreter lock exists or should be introduced.
* the core library must not initialize CPython.

Codon supports native compiled execution, so there is no reason for the CARLA path to acquire a Python GIL.

## 18. Python API compatibility policy

Compatibility is semantic, not binary.

Preferred API:

```python
import typesafe_carla as carla
```

Existing code:

```python
client = carla.Client("localhost", 2000)
world = client.get_world()
```

should usually remain identical.

Differences are acceptable when necessary for static safety. For example, Python CARLA code may use:

```python
actor = world.get_actor(id)
actor.apply_control(...)
```

typesafe_carla may require:

```python
actor = world.get_actor(id)
vehicle = actor.as_vehicle()
vehicle.apply_control(...)
```

because static typing cannot safely assume every Actor is a Vehicle.

Static safety takes priority over exact dynamic API compatibility.

## 19. No implicit Any / pyobj policy

The public API should not use a universal dynamic object type.

Avoid:

```python
def get_attribute(name: str):
    ...
```

returning arbitrary dynamically typed values where possible.

Prefer:

```python
attribute = blueprint.get_attribute("color")
attribute.as_string()
attribute.as_int()
attribute.as_float()
attribute.as_bool()
```

or typed domain-specific methods.

Any introduction of pyobj into the normal path should be considered an architecture violation.

## 20. Package layout

Recommended repository:

```
typesafe_carla/
├── README.md
├── LICENSE
├── pyproject.toml
├── uv.lock
├── CMakeLists.txt
├── codon/
│   └── typesafe_carla/
│       ├── __init__.codon
│       ├── client.codon
│       ├── world.codon
│       ├── actor.codon
│       ├── vehicle.codon
│       ├── sensor.codon
│       ├── blueprint.codon
│       ├── geometry.codon
│       ├── control.codon
│       ├── command.codon
│       ├── traffic_manager.codon
│       ├── errors.codon
│       └── _ffi.codon
├── native/
│   ├── include/
│   │   └── typesafe_carla/
│   │       └── ffi.h
│   └── src/
│       ├── client.cpp
│       ├── world.cpp
│       ├── actor.cpp
│       ├── vehicle.cpp
│       ├── sensor.cpp
│       ├── blueprint.cpp
│       ├── traffic_manager.cpp
│       ├── error.cpp
│       └── handle.cpp
├── python/
│   └── typesafe_carla/
│       ├── __init__.py
│       ├── cli.py
│       ├── toolchain.py
│       └── paths.py
├── tests/
│   ├── compile/
│   ├── unit/
│   ├── integration/
│   └── compatibility/
└── examples/
    ├── connect.py
    ├── spawn_vehicle.py
    ├── vehicle_control.py
    ├── camera.py
    └── synchronous_mode.py
```

Codon supports normal module/package layouts including `__init__.codon`, and distributing Codon source while adding its directory to `CODON_PATH` is currently the practical approach for reusable Codon modules.

## 21. Distribution model

Two packages should eventually be published:

```
typesafe-carla
typesafe-carla-toolchain
```

Import name remains `import typesafe_carla`. PyPI distribution names are separate from the source import name.

## 22. typesafe-carla package

Contains:

* Codon source
* `libtypesafe_carla_ffi.so`
* Python bootstrap/launcher
* metadata
* license files

It does NOT require the CARLA Python package.

Runtime dependency graph:

```
typesafe-carla
├── typesafe-carla-toolchain
└── bundled native FFI
        └── LibCarla
```

## 23. Codon toolchain package

typesafe-carla-toolchain contains a pinned Codon distribution.

Initial support: Linux x86_64 only. Later: Linux aarch64, macOS arm64, macOS x86_64.

The first target should match CARLA development environments rather than attempting universal support.

Codon v0.18 and later is Apache-2.0 licensed, so redistribution is technically compatible with a toolchain packaging model, subject to preserving required license notices.

The toolchain wheel should expose a launcher named `codon` or preferably `typesafe-codon`.

The launcher sets `CODON_DIR`, `CODON_PATH` and `LD_LIBRARY_PATH` before executing the bundled Codon compiler.

No post-install hook is required.

This is important: `uv sync` should only install wheel contents. Installation must not perform arbitrary `curl | bash` side effects.

## 24. uv experience

Target downstream project:

```toml
[project]
name = "my-carla-project"
version = "0.1.0"
dependencies = [
    "typesafe-carla==0.1.*",
]
```

Then `uv sync` installs:

* Codon compiler
* typesafe_carla Codon source
* typesafe_carla native FFI
* all Python-side tooling

Build:

```sh
uv run typesafe-codon build -release main.py
```

Run directly:

```sh
uv run typesafe-codon run -release main.py
```

Desired final UX:

```sh
git clone my-project
cd my-project
uv sync
uv run typesafe-codon run main.py
```

No global Codon installation should be required in the final distribution model.

uv supports build systems, native-extension-oriented backends, project scripts, wheels, and locked dependencies, so it is suitable as the outer dependency/environment manager.

## 25. CODON_PATH management

The user should not manually configure `CODON_PATH`.

`typesafe-codon` should compute `<venv>/site-packages/typesafe_carla/_codon` and prepend it to `CODON_PATH` before invoking the real Codon compiler.

```python
def main():
    codon_dir = locate_bundled_codon()
    typesafe_path = locate_typesafe_carla_codon_modules()
    env = os.environ.copy()
    env["CODON_DIR"] = str(codon_dir)
    env["CODON_PATH"] = join_paths(typesafe_path, env.get("CODON_PATH"))
    exec_codon(sys.argv[1:], env)
```

This avoids requiring users to understand Codon's package search paths.

## 26. FFI library loading

Codon code should dynamically load `libtypesafe_carla_ffi.so`.

Example `_ffi.codon`:

```python
LIB = "/absolute/resolved/path/libtypesafe_carla_ffi.so"
from C import LIB.tsc_client_create(...) -> ...
from C import LIB.tsc_client_destroy(...) -> None
```

Codon supports dynamic shared-library loading for typed C imports.

The launcher can expose the shared-library location using an environment variable, `TYPESAFE_CARLA_LIB`. The Codon module reads this during initialization.

Alternative later implementation: link `libtypesafe_carla_ffi` directly into the final executable.

Dynamic loading should be used initially because packaging is simpler.

## 27. CARLA / LibCarla versioning

typesafe_carla must define a supported LibCarla version.

```
typesafe_carla 0.1
Codon 0.18.x
CARLA 0.9.x / selected commit
Linux x86_64
```

Do not claim broad compatibility without testing it.

Native ABI functions:

```c
uint32_t tsc_abi_version();
const char* tsc_libcarla_version();
const char* tsc_build_commit();
```

At runtime, `Client(...)` may compare the typesafe_carla build version and the CARLA server version and issue a warning or error when outside the supported matrix.

## 28. Internal FFI ABI version

The C API itself must be versioned independently.

```c
#define TSC_ABI_VERSION_MAJOR 1
#define TSC_ABI_VERSION_MINOR 0
```

Runtime: `uint32_t tsc_abi_version();`

This allows the Codon layer and native library to detect mismatched installations immediately.

## 29. MVP API coverage

Phase 1 should intentionally cover the APIs required for normal closed-loop simulation.

**Client:** Client, set_timeout, get_world, load_world, reload_world, get_server_version, get_client_version, apply_batch, apply_batch_sync, get_trafficmanager

**World:** get_map, get_settings, apply_settings, tick, wait_for_tick, get_snapshot, get_actors, get_actors(actor_ids), get_actor, get_blueprint_library, spawn_actor, try_spawn_actor, destroy_actor

**Geometry:** Location, Rotation, Transform, Vector2D, Vector3D, BoundingBox

**Actor:** id, type_id, get_transform, set_transform, get_location, get_velocity, set_target_velocity, get_acceleration, get_angular_velocity, destroy

**Vehicle:** apply_control, get_control, set_autopilot, get_physics_control, apply_physics_control

**Vehicle control:** VehicleControl, VehiclePhysicsControl, WheelPhysicsControl

**Blueprint:** BlueprintLibrary, ActorBlueprint, ActorAttribute, filter, find, has_attribute, get_attribute, set_attribute

**Sensors** (first, with polling/queue semantics): Camera, LiDAR, IMU, GNSS, Collision

**Traffic Manager** (basic): set_synchronous_mode, set_random_device_seed, vehicle_percentage_speed_difference, distance_to_leading_vehicle, auto_lane_change, random_left_lanechange_percentage, random_right_lanechange_percentage

## 30. Batch commands

CARLA batch operations are important for simulation performance.

Do not implement batches as repeated individual RPC calls. Expose native command representations.

```python
commands = [
    SpawnActor(...),
    DestroyActor(...),
    ApplyVehicleControl(...),
]
responses = client.apply_batch_sync(commands, True)
```

Internally:

```
Codon typed command
        ↓
C-compatible command representation
        ↓
LibCarla command variant
        ↓
single batch RPC
```

Command representation should use a tagged union.

```c
enum tsc_command_type {
    TSC_SPAWN_ACTOR,
    TSC_DESTROY_ACTOR,
    TSC_APPLY_CONTROL
};
```

## 31. Synchronous simulation

Synchronous mode is a priority feature because many autonomous-driving stacks depend on deterministic stepping.

```python
settings = world.get_settings()
settings.synchronous_mode = True
settings.fixed_delta_seconds = 0.05
world.apply_settings(settings)
while True:
    frame = world.tick()
```

Types:

```python
class WorldSettings:
    synchronous_mode: bool
    no_rendering_mode: bool
    fixed_delta_seconds: Optional[float]
    substepping: bool
    max_substep_delta_time: float
    max_substeps: int
```

## 32. Compile-time tests

typesafe_carla must test both successful and intentionally failing programs.

Valid: `vehicle.apply_control(VehicleControl(throttle=0.5))` must compile.

Invalid: `vehicle.apply_control(Transform())` must fail compilation.

CI should maintain directories `tests/compile/pass/` and `tests/compile/fail/`.

Examples:

```
pass/client.codon
pass/vehicle_control.codon
pass/transform.codon
fail/control_with_transform.codon
fail/set_transform_string.codon
fail/wrong_client_port.codon
```

A compile-fail test is a first-class feature of this project, not merely an incidental test.

## 33. Runtime compatibility tests

Run the same conceptual operations against the official CARLA Python API and typesafe_carla, and compare results:

```
connect
get map
spawn vehicle
read transform
apply control
tick simulation
destroy vehicle
```

The compatibility suite should compare semantic output rather than internal representation.

## 34. CI

Recommended jobs:

* lint
* C++ build
* Codon compile-pass tests
* Codon compile-fail tests
* unit tests
* CARLA integration tests
* wheel build
* wheel install test
* uv clean-environment test

Most important packaging test:

```sh
git clone typesafe_carla
uv sync --frozen
uv run typesafe-codon run examples/connect.py
```

The clean CI environment must not have Codon or the CARLA Python package preinstalled.

This verifies that the distribution is genuinely self-contained.

## 35. Build system

Recommended native build system: CMake, because LibCarla itself is C++ and CARLA development already uses CMake extensively.

Python/wheel build: scikit-build-core is a good candidate because the project contains C++ native code and CMake; uv explicitly supports projects using native-extension-oriented build backends such as scikit-build-core.

Conceptual `pyproject.toml`:

```toml
[build-system]
requires = ["scikit-build-core"]
build-backend = "scikit_build_core.build"

[project]
name = "typesafe-carla"
version = "0.1.0"
description = "A statically typed native CARLA client for Codon"
requires-python = ">=3.10"
dependencies = ["typesafe-carla-toolchain==0.1.*"]

[project.scripts]
typesafe-codon = "typesafe_carla.cli:main"
```

Exact dependencies and versions are finalized after the first working toolchain wheel.

## 36. Native library packaging

Preferred wheel contents:

```
site-packages/
└── typesafe_carla/
    ├── _codon/
    │   └── typesafe_carla/
    │       ├── __init__.codon
    │       └── ...
    ├── _native/
    │   ├── libtypesafe_carla_ffi.so
    │   └── dependent shared libraries
    ├── cli.py
    └── __init__.py
```

Use `$ORIGIN`-relative RPATH where possible so that shared dependencies can be located without modifying the host system.

Do not install libraries globally into `/usr/lib` or `/usr/local/lib`.

## 37. Development workflow

```sh
uv sync
uv run cmake -S . -B build
uv run cmake --build build
uv run typesafe-codon run examples/connect.py
uv run typesafe-codon build -release examples/connect.py
```

The same launcher should be used locally and by downstream users so differences between development and packaged environments remain minimal.

## 38. API generation

Manually implementing the entire CARLA API is error-prone.

Long term, generate portions of the binding from LibCarla headers.

```
LibCarla headers
      ↓
Clang AST
      ↓
typesafe_carla IR
      ├── C shim declarations
      ├── C++ shim implementations
      └── Codon FFI declarations
```

Do not expose the raw generated API directly. Use generation for repetitive plumbing, while maintaining handwritten high-level wrappers.

```
generated/
    actor_ffi.cpp
    actor_ffi.codon
public/
    actor.codon
```

## 39. Binding metadata

A declarative binding specification can eventually become the source of truth.

```yaml
Vehicle:
  base: Actor
  methods:
    get_transform:
      returns: Transform
    set_transform:
      args:
        transform: Transform
      returns: None
    apply_control:
      args:
        control: VehicleControl
      returns: None
```

Generator output: C header, C++ wrapper, Codon FFI declaration, basic tests.

This reduces drift between layers.

## 40. Performance goals

The FFI itself should introduce negligible overhead relative to CARLA RPC/network costs.

Avoid:

* CPython calls
* Python object allocation
* JSON serialization
* reflection
* dynamic dispatch through strings
* unnecessary copies

Preferred hot path:

```
Codon native values
        ↓
C ABI
        ↓
C++ call
        ↓
LibCarla
```

For sensor data:

```
LibCarla memory
        ↓
native shared ownership
        ↓
zero-copy Codon view
```

where safe.

## 41. Safety boundary

The static safety guarantee is:

```
User Codon application      STATIC
        ↓
typesafe_carla API          STATIC
        ↓
typesafe_carla FFI types    STATIC
        ↓
--------------------------------
C ABI boundary
--------------------------------
        ↓
LibCarla C++                NATIVE
```

The compiler can guarantee API type compatibility above the native boundary.

It cannot prove:

* CARLA server state
* whether a requested actor exists
* network availability
* server/client semantic compatibility
* whether an actor runtime type matches a requested cast
* simulation-domain constraints

Those remain runtime errors.

This distinction should be documented clearly.

## 42. Example of the resulting benefit

Existing dynamic Python:

```python
vehicle.apply_control(carla.Transform(...))
```

fails only after execution reaches this line.

With typesafe_carla:

```python
vehicle.apply_control(Transform(...))
```

fails during compilation because:

```
Vehicle.apply_control:
    expected VehicleControl
    received Transform
```

Likewise:

```python
def control(vehicle: Vehicle, command: VehicleControl):
    vehicle.apply_control(command)
```

provides a typed contract across library boundaries.

If a downstream planner library is also compiled with Codon:

```
application
    ↓
planner library
    ↓
controller library
    ↓
typesafe_carla
```

the complete chain can be type-checked before running CARLA.

## 43. Initial milestone plan

### Milestone 0 — proof of concept

Implement only: Client, World, Actor, Vehicle, Location, Rotation, Transform, VehicleControl.

Native calls: connect, get_world, get_actors, get_transform, set_transform, apply_control.

Success criterion:

```python
import typesafe_carla as carla
client = carla.Client("localhost", 2000)
world = client.get_world()
vehicle = world.get_actors()[0].as_vehicle()
vehicle.apply_control(
    carla.VehicleControl(
        throttle=0.2,
        steer=0.0,
    )
)
```

works without CPython or carla Python package.

### Milestone 1 — usable vehicle API

Add: BlueprintLibrary, spawn_actor, destroy, WorldSettings, tick, snapshot, VehiclePhysicsControl, Map, Waypoint, batch commands.

Goal: basic autonomous-driving experiments can use typesafe_carla.

### Milestone 2 — sensors

Add: camera, LiDAR, GNSS, IMU, collision, sensor queues, zero-copy buffers.

Goal: full closed-loop perception/control experiments.

### Milestone 3 — distribution

Provide: typesafe-carla wheel, typesafe-carla-toolchain wheel, uv integration, CODON_PATH launcher, LibCarla bundling.

Goal: `uv sync && uv run typesafe-codon run main.py` works on a clean Linux x86_64 machine.

### Milestone 4 — broader CARLA compatibility

Add: TrafficManager, Walker, TrafficLight, Recorder, DebugHelper, Weather, OpenDRIVE, map queries, remaining commands.

### Milestone 5 — binding generation

Introduce: LibClang parser, binding schema, generated C ABI, generated Codon FFI, compatibility coverage report.

## 44. Success criteria

typesafe_carla should be considered successful when the following workflow works:

```sh
git clone application
cd application
uv sync
uv run typesafe-codon build -release main.py
./main
```

with:

* no system Python CARLA package
* no manually installed Codon
* no manually configured CODON_PATH
* no manual LibCarla installation

and code such as `vehicle.apply_control(Transform())` fails before the executable is produced.

## 45. Core architectural decisions

| Decision | Choice |
|---|---|
| Project name | typesafe_carla |
| Primary language | Codon |
| Native backend | LibCarla C++ |
| Interop | C ABI |
| Python runtime dependency | none for CARLA execution |
| Python packaging | yes |
| Package manager | uv |
| Distribution | wheel |
| Codon package format | source modules bundled in wheel |
| Codon package discovery | automatic CODON_PATH via launcher |
| Codon compiler | pinned bundled toolchain |
| Initial OS | Linux x86_64 |
| Sensor concurrency | C++ queue + Codon polling |
| Error boundary | status code + thread-local error |
| Object boundary | opaque reference-counted handles |
| Value boundary | POD values/output pointers |
| Primary compatibility target | CARLA Python API naming and semantics |
| Priority | static safety > exact dynamic compatibility |

## 46. Final architecture

```
                         uv.lock
                            │
                            ▼
                     ┌─────────────┐
                     │   uv sync   │
                     └──────┬──────┘
                            │
             ┌──────────────┴──────────────┐
             ▼                             ▼
 typesafe-carla-toolchain             typesafe-carla
             │                             │
             │ Codon                       │ Codon modules
             │ compiler                    │ native FFI
             └──────────────┬──────────────┘
                            │
                            ▼
                  typesafe-codon launcher
                            │
              CODON_DIR / CODON_PATH / RPATH
                            │
                            ▼
                  Codon user application
                            │
                    static type check
                            │
                            ▼
                    typesafe_carla
                            │
                      typed C ABI
                            │
                            ▼
              libtypesafe_carla_ffi.so
                            │
                            ▼
                        LibCarla
                            │
                            ▼
                       CARLA Server
```

The fundamental rule of the project is:

> typesafe_carla should make incorrect CARLA API usage a compilation problem whenever that error can be represented in the type system, while preserving CARLA's familiar Python-style API and avoiding CPython on the execution path.
