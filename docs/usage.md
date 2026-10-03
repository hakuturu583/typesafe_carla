# Using typesafe_carla in your project

## Install

```toml
# pyproject.toml of your application
[project]
name = "my-carla-project"
version = "0.1.0"
requires-python = ">=3.10"
dependencies = ["typesafe-carla==0.1.*"]
```

```sh
uv sync                                   # installs typesafe-carla and a pinned Codon
uv run typesafe-codon info                # shows the Codon, the native library and the CARLA ref
uv run typesafe-codon run main.py         # compile and run
uv run typesafe-codon build -release -o main main.py && ./main
```

You don't need:
- a separate Codon installation;
- `CODON_PATH`;
- a LibCarla installation;
- the CARLA Python package.

`typesafe-codon build` does need a system C++ compiler and zlib (Ubuntu:
`apt install g++ zlib1g-dev`), because Codon links executables with
`g++ ... -lz`. `run` does not.

The executable produced by `build` finds the native library through its
RPATH. It runs without the launcher, as long as the virtual environment it
was built from still exists.

## Which CARLA

The published wheel contains LibCarla built from a CARLA UE5 ref.
`typesafe-codon info` shows which one, under `carla ref` and `carla commit`.
Use a client built from the same ref as your server (for example `0.10.0`
for a CARLA 0.10.0 server). To build one for another ref, have uv build
typesafe-carla from its source distribution:

```toml
[tool.uv]
no-binary-package = ["typesafe-carla"]
```

```sh
CARLA_GIT_REF=0.10.0 uv sync --reinstall-package typesafe-carla
```

`--reinstall-package` forces a rebuild: uv's build cache does not know about
`CARLA_GIT_REF`. The build fetches CARLA and compiles LibCarla, so it needs
git, a C++20 compiler, network access to GitHub and some time. CMake is
installed automatically if the system one is older than 3.27.2.

## Writing programs

Programs are Codon. They look like CARLA Python API programs, with checked
conversions where the Python API relies on dynamic typing:

```python
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
world = client.get_world()
bp = world.get_blueprint_library().find("vehicle.lincoln.mkz")
vehicle = world.spawn_actor(bp, world.get_map().get_spawn_points()[0]).as_vehicle()
vehicle.apply_control(carla.VehicleControl(throttle=0.5))

camera = world.spawn_actor(world.get_blueprint_library().find("sensor.camera.rgb"),
                           carla.Transform(carla.Location(1.5, 0.0, 2.0)),
                           attach_to=vehicle).as_sensor()
camera.listen()
image = camera.wait_for_data(5.0).as_image()   # polling mode

# Or a callback, as in the Python API. It runs on this program's thread, never
# on a CARLA thread: inside world.tick() / world.wait_for_tick(), or when the
# program calls carla.dispatch_sensor_callbacks().
frames = List[int]()
camera.listen(lambda data: frames.append(data.as_image().frame))
world.wait_for_tick()   # runs the callbacks queued so far
```

Python-API code that calls subclass methods on whatever `get_actor` returns
also works: `world.get_actor(i).apply_control(...)` converts at run time and
raises `ActorTypeError` if the actor is not a vehicle. Such shortcuts are not
statically checked for the actor kind, so the launcher warns about each one at
compile time and the program warns the first time it takes one
(`TYPESAFE_CARLA_COMPAT_WARNINGS=0` silences both). With
`typesafe-codon --strict ...` (or `TYPESAFE_CARLA_STRICT=1`) they are compile
errors, and only the `as_vehicle()` & co. path compiles.

To print the compile-time warnings before the program's output without a
second compilation, `typesafe-codon run` builds the program (Codon links with
g++) and then execs it, so the launcher process becomes the program: same
PID, signals and exit status. Without g++, or where the cache directory is
mounted noexec, it falls back to Codon's JIT and a separate warning pass.

A mistake such as `vehicle.apply_control(carla.Transform())` is a compile
error, and no executable is produced:

```
error: 'Transform' does not match expected type 'VehicleControl'
```

Lookups that can miss return `None` in the same cases as the Python API, and
the usual Python idioms work:

```python
actor = world.get_actor(actor_id)
if actor is None:
    print("no such actor")
left = world.get_map().get_waypoint(location).get_left_lane()
if left:
    print(left.lane_id)
world.try_spawn_actor(bp, transform).destroy()   # raises ValueError if it is None
```

As in the Python API, `Location` derives from `Vector3D`: it has `length()`,
`dot()`, `cross()`, `make_unit_vector()`, `distance_2d()` and the rest, and
its arithmetic is Vector3D's: `Location - Location` is a `Vector3D`, as in the
CARLA 0.10.0 Python API. A `Location` is accepted where the API
takes a `Vector3D` (a velocity, force, impulse or direction), but the program
prints a one-time warning, because a position passed as a velocity is usually
a mistake. A displacement is already a `Vector3D`; convert a `Location`
explicitly to keep the program warning-free:

```python
vehicle.set_target_velocity(target - vehicle.get_location())   # Vector3D: fine
vehicle.set_target_velocity(target)                             # Location: warns
vehicle.set_target_velocity(target.as_vector())                 # explicit: fine
```

The reverse also holds, as in the Python API: a `Vector3D` (for example
`loc + offset`) is accepted where the API takes a `Location` (`Transform`,
`set_location`, `get_waypoint`, the debug drawing functions, ...), with the
same kind of warning. Convert it with `carla.Location(v)` to keep the program
warning-free:

```python
carla.Transform(carla.Location(spawn.location + offset), spawn.rotation)
```

Assignments do not convert. Once `loc` is a `Location` variable,
`loc = loc + offset` (in a loop, or in one branch of an `if`) is a compile
error (`'Vector3D' does not match expected type 'Location'`), and so are
`t.location = loc + offset` and `loc /= k` (which rebinds `loc` to a
`Vector3D` in the Python API). `loc += offset`, `loc -= offset` and
`loc *= k` update `loc` in place and keep it a `Location`, as in the Python
API. Otherwise write `loc = carla.Location(loc + offset)`, or keep a vector
and convert at the end:

```python
pos = actor.get_location().as_vector()
for step in range(10):
    pos += offset
actor.set_location(carla.Location(pos))
```

As with the other compatibility paths, the launcher also warns at compile
time, `TYPESAFE_CARLA_COMPAT_WARNINGS=0` silences the warnings, and
`typesafe-codon --strict` turns them into compile errors.

The README covers the API surface and the Codon-specific caveats: exceptions
do not form a hierarchy, `Optional` is unwrapped implicitly (with one gap where
an explicit `unwrap()` is needed), and only called functions are type-checked.
