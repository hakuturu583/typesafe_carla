# Using typesafe_carla in your project

## Install

```toml
# pyproject.toml of your application
[project]
name = "my-carla-project"
version = "0.1.0"
requires-python = ">=3.11"
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

`typesafe-codon build` does need a system C compiler (`cc`), because Codon links executables with it. `run` does not.

The executable produced by `build` finds the native library through its
RPATH. It runs without the launcher, as long as the virtual environment it
was built from still exists.

## Which CARLA

The published wheel contains LibCarla built from a CARLA UE5 ref.
`typesafe-codon info` shows which one, under `carla ref` and `carla commit`.
Use a client built from the same ref as your server (for example `0.10.0`
for a CARLA 0.10.0 server). To build one for another ref, build from source:

```sh
CARLA_GIT_REF=0.10.0 uv pip install --no-binary typesafe-carla typesafe-carla
```

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
image = camera.wait_for_data(5.0).as_image()   # polling; no callbacks on CARLA threads
```

A mistake such as `vehicle.apply_control(carla.Transform())` is a compile
error, and no executable is produced:

```
error: 'Transform' does not match expected type 'VehicleControl'
```

The README covers the API surface and the Codon-specific caveats: exceptions
do not form a hierarchy, `Optional` is unwrapped implicitly, and only called
functions are type-checked.
