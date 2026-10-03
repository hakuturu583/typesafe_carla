# CARLA's own PythonAPI tests against typesafe_carla

`tools/upstream_tests.py` takes CARLA's test suite (`PythonAPI/test/{unit,smoke,API}`
of carla-simulator/carla) from the CARLA commit the native library was built
from. It converts each module mechanically, compiles it with `typesafe-codon build`
and runs it one test at a time. `expectations.yaml` records the expected result
per CARLA ref. `tests/test_upstream.py` (pytest) and the `libcarla` CI legs fail
on any difference from it, an unexpected pass included, so the list only shrinks.

| suite | needs | runs in |
|---|---|---|
| `unit` | nothing | pytest (any backend; the mock uses ue5-dev's tests), every `libcarla` CI leg |
| `smoke`, `API` | a CARLA server and the `libcarla` backend | pytest and the CLI, only with `TSC_CARLA_PORT` set |

## Running

```sh
# unit, against whatever library the checkout uses (build/ = mock by default)
uv run python -m tools.upstream_tests --suite unit -v
uv run pytest tests/test_upstream.py

# against a real LibCarla build and a server (a 0.10.0 server needs a 0.10.0 build)
export TYPESAFE_CARLA_BUILD_DIR=build-carla TSC_CARLA_HOST=127.0.0.1 TSC_CARLA_PORT=2000
uv run python -m tools.upstream_tests --suite smoke --suite API -j1 -v
uv run pytest tests/test_upstream.py -s

# smoke/API without a server: compile only (finds the tests that cannot compile)
uv run python -m tools.upstream_tests --suite smoke --suite API --compile-only -v
```

Use `127.0.0.1` rather than `localhost` if `localhost` resolves to `::1` and the
server listens on IPv4 only. Run the server suites with `-j1`: the tests load
maps and change world settings, so two files must not share a server at once.
The smoke tests restore the world in `tearDown`. A test that fails before that
can leave actors or synchronous mode behind.

| variable | meaning |
|---|---|
| `TSC_UPSTREAM_REF` | CARLA ref whose expectations (and, if it differs from the build's, tests) to use. Default: the ref the library was built from (`TSC_CARLA_REF_NAME`). For a build from a local checkout, it is the manifest ref whose current commit matches, else the CARLA version. The mock uses `ue5-dev`. |
| `TSC_UPSTREAM_TESTS_DIR` | use a local `PythonAPI/test` instead of fetching |
| `TSC_UPSTREAM_CACHE` | where fetched tests and builds go (default `<build dir>/upstream-tests/<sha>`) |
| `TSC_CARLA_HOST`, `TSC_CARLA_PORT` | the server for `smoke` and `API` |

The tests are fetched once per commit SHA, as a shallow, blob-filtered, sparse
checkout of `PythonAPI/test` only.

## Updating the expectations

```sh
uv run python -m tools.upstream_tests --suite unit --update                   # this ref's unit results
uv run python -m tools.upstream_tests --suite smoke --suite API --update -j1  # with a server
uv run python -m tools.upstream_tests --suite smoke --suite API --compile-only --update
```

`--update` rewrites the current ref's entries from the run, and keeps the
reasons of tests that still fail. With `--compile-only`, it records only
compile failures, and a test that compiles keeps its entry. A test the
manifest does not list is expected to pass. That is how the three API tests
that compile stand until they are run against a server.

Entry forms, per ref:

```yaml
refs:
  0.10.0:
    unit/test_client.py: "xfail: compile: ..."    # the whole module does not compile
    unit/test_transform.py:
      TestTransform.test_values: pass
      TestTransform.test_print: "xfail: fail: ..."
      TestX.test_y: "skip: crashes the server"    # never run
```

An `xfail` reason starts with the outcome: `compile` (with the first compiler
error, at the upstream line the test reached it from), `fail` (an assertion),
`error` (an exception), `crash` (a signal) or `timeout`.

## The conversion

The conversion is the same for every module. There are no per-test edits.

- `import carla` becomes `import typesafe_carla as carla`, the Python-API
  compatibility path. The compile-time compatibility warnings are expected.
  `from carla import X` is rewritten the same way.
- `import unittest` becomes [`tsc_unittest.codon`](tsc_unittest.codon). Codon's
  bundled `unittest` only prints a failed assertion and goes on, and has no
  setUp/tearDown, `skipTest` or discovery. The shim raises `AssertionError` and
  `SkipTest`, and has the assertions the upstream tests use.
- Some Python spellings Codon lacks are rewritten to forms with the same
  meaning:
  - `super(Class, self)` becomes `super()`;
  - `from __future__` imports are dropped;
  - `__file__` is defined as the upstream path;
  - the egg-locator boilerplate (`try: sys.path.append(glob.glob('../carla/dist/carla-*.egg')[0])`)
    and its `import glob` are dropped. That code finds the `carla` module, which
    the conversion has replaced.
- For `smoke` and `API` only, the server address is redirected to
  `TSC_CARLA_HOST`/`TSC_CARLA_PORT`. This covers `('localhost', <port>)`
  literals, `carla.Client()` with no arguments and `carla.Client('localhost', <port>)`.
- A runner is appended. Test classes and `test_*` methods, inherited ones
  included, are found from the Python AST. `<exe> Class.test_method` runs
  `setUp`, the test and `tearDown`, then prints the outcome. Each test runs in
  its own process, as unittest makes a fresh instance per test, so a crash
  fails only that test.

Codon type-checks only functions that are called. A test that does not compile
is dropped from the runner and the module is compiled again, so the other tests
still run. An error that no test's call chain reaches, i.e. module-level code,
fails the whole file.

## What does not compile, and why

These are the incompatibilities the suite shows, grouped by root cause, as of
ue5-dev 1360bb9 and 0.10.0. Each test is listed under its first compiler
error only. The section "Seen behind the first error" lists features that are
present but not yet confirmed by the compiler.

**Fix belongs in typesafe_carla:**

- **No `==` on the geo value types.** `GeoOffsetTransform`, `GeoProjectionUTM`
  and `GeoEllipsoid` define no `__eq__`; `GeoLocation` does. This causes 6
  ue5-dev `unit/test_transform.py` tests (`test_geo_offset_transform_equality`,
  `test_geo_projection_utm_{with_offset,offset_setter,equality,constructor_3_args,constructor_3_args_positional}`).
  The official module compares them by value.
- **`str()` of value types formats floats differently.** typesafe_carla prints
  `Location(x=1, ...)`; CARLA prints `Location(x=1.000000, ...)` (`%f`). This
  fails `TestTransform.test_print` on both refs (a runtime failure, not a
  compile error). Codon's `f"{x:.6f}"` needs a locale (see CLAUDE.md), so the fix
  needs its own fixed-point formatter.
- **No `VehiclePhysicsControl(torque_curve=[[x, y], ...])`.** The official API
  converts a list of pairs to `Vector2D`; typesafe_carla takes
  `List[Vector2D]` only.

**Upstream test is stale:**

- `unit/test_vehicle.py` `TestVehiclePhysicsControl.test_named_args` uses
  UE4-era fields (`tire_friction`, `radius`, `moi`, `use_gear_autobox`,
  `gear_switch_time`, `WheelPhysicsControl.position`) that the official 0.10.0
  module lacks too. Its first error is the Codon one below (a mixed int/float
  list literal).

**Codon language (dynamic Python features):**

- **Instance attributes created outside `__init__`.** `SmokeTest.setUp` (in
  `smoke/__init__.py`) sets `self.client`, `self.world`, ..., and `tearDown`
  sets them back to `None`. Codon needs class fields known statically. This
  blocks every smoke test, as they are all based on `SmokeTest`/`SyncSmokeTest`.
  39 tests on ue5-dev and 11 on 0.10.0 are recorded under it; the other smoke
  files fail earlier, on a missing module. Turning `setUp` into `__init__` would
  not help: Codon's field inference does not compose with `super().__init__()`,
  and `None` cannot be assigned to a `World` field.
- **Mixed int/float list literals** (`[[0, 400], [1315.47, 654.445]]`): the
  inner lists get different types.
- **`super(Class, self)`**: rewritten by the harness.

**Codon standard library (missing modules or names):**

| missing | used by |
|---|---|
| `subprocess` | `unit/test_client.py` |
| `ast` | `unit/test_lidar_smoke_helpers.py` |
| `os.path.abspath` | `unit/test_boost_version.py`, `unit/test_numpy_compat.py` |
| `enum` | `smoke/test_lidar.py`, `smoke/test_vehicle_physics.py` |
| `queue` | `smoke/test_sync.py`, `smoke/test_encoding_cameras.py` |
| `filecmp` | `smoke/test_collision_determinism.py`, `smoke/test_sensor_determinism.py` |
| `tempfile` | `smoke/test_recorder.py`, `smoke/test_replay_no_actor_aliasing.py` |
| `array` | `smoke/test_v2x.py` |
| `numpy.random` | `smoke/test_determinism.py` |
| `argparse` (module level) | `API/test_apply_textures.py`, `API/test_collision.py`, `API/test_semantic_segmentation.py` |
| `glob` (used, not only the egg locator) | `API/test_no_rendering_mode.py`, `API/test_sensor_recording.py`, `API/test_sensor_recording_fast.py` |

`unit/test_boost_version.py`, `test_numpy_compat.py` and
`test_lidar_smoke_helpers.py` check CARLA's repository files, not the API.
They stay `xfail` here.

**Seen behind the first error (static survey, not compiler-confirmed):**

- `np.frombuffer(image.raw_data, ...)`: `raw_data` is a buffer property
  upstream, a `raw_data()` method returning `Ptr[u8]` here. Used by
  `smoke/test_lidar.py`, `test_sensor_determinism.py` and `test_encoding_cameras.py`.
- `isinstance(x, (carla.GeoProjectionTM, carla.GeoProjectionUTM, ...))`, and
  `carla.libcarla.Vector3D`: `smoke/test_geoconversion.py`, `test_vehicle_physics.py`.
- `getattr(obj, name)` / `getattr(self, name, None)`: `smoke/test_vehicle_physics.py`,
  `test_encoding_cameras.py`, `test_replay_no_actor_aliasing.py`.
- `with self.assertRaises(RuntimeError):`, the context-manager form. The shim
  cannot catch inside `with`. Used by `smoke/test_actor_introspection.py`.
- `listen(lambda data, i=i: ...)`, `listen(queue.put)`, `listen(self.method)`,
  and `weakref.ref(self)` in callbacks: smoke/API sensor tests.
- `threading.Thread`: `smoke/test_streamming.py`.
