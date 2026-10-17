"""tools/pycarla's CPython package against the mock backend.

Issue #89: as in CARLA's Python API, sensor callbacks run on a background
thread as soon as their data arrives, so a program may tick and then block on
its own queue (upstream smoke/test_lidar.py). The mock delivers each tick's
data late, on a worker thread (`tsc_mock_set_delivery_delay_ms`, issue #86),
as LibCarla's streaming threads can.

Needs the package built and up to date (`python -m tools.pycarla`, ~15 min),
or TSC_UPSTREAM_BUILD_PYCARLA=1 to build it; CI's mock job builds (and
caches) it, so these run there. Mock backend only (its delivery-delay hook,
and a world without a server).
"""

from __future__ import annotations

import os
import subprocess
import time

import pytest

from tools import upstream_tests as up


@pytest.fixture(scope="module")
def pycarla(backend):
    if backend != "mock":
        pytest.skip(f"needs the mock backend's delivery-delay hook (built: {backend})")
    pydir = up.pycarla_dir(build=os.environ.get("TSC_UPSTREAM_BUILD_PYCARLA") == "1")
    if pydir is None:
        pytest.skip("needs the carla package: run `python -m tools.pycarla` "
                    "or set TSC_UPSTREAM_BUILD_PYCARLA=1")
    return pydir


def _run_ok(pydir, program: str, timeout: float = 120, env: dict | None = None) -> str:
    """Runs `program` with the generated `carla` (`env`: added to its
    environment); checks it printed only OK and returns its stderr."""
    # As the upstream-test driver: load the generated package explicitly (an
    # installed official `carla` must not win).
    head = (
        "import importlib.util, os, sys\n"
        "pkg = os.environ['TSC_PYCARLA_PKG']\n"
        "spec = importlib.util.spec_from_file_location('carla', os.path.join(pkg, '__init__.py'),"
        " submodule_search_locations=[pkg])\n"
        "carla = importlib.util.module_from_spec(spec); sys.modules['carla'] = carla\n"
        "spec.loader.exec_module(carla)\n")
    result = subprocess.run([up._python(), "-c", head + program],
                            env={**up._cpython_env(pydir, False), **(env or {})},
                            capture_output=True, text=True, timeout=timeout)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.split() == ["OK"], result.stdout
    return result.stderr


_BLOCKING_QUEUE = """
import ctypes, queue, threading, time
ctypes.CDLL(os.environ['TYPESAFE_CARLA_LIB']).tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(50))
world = carla.Client('localhost', 2000).get_world()
lib = world.get_blueprint_library()
vehicle = world.get_actors()[0]
main = threading.get_ident()
q = queue.Queue()
lidar = world.spawn_actor(lib.find('sensor.lidar.ray_cast'), carla.Transform(), attach_to=vehicle)
lidar.listen(lambda data: q.put((data.frame, threading.get_ident())))
for _ in range(3):
    frame = world.tick()
    # The upstream pattern: block on the queue, no tick in between. The data
    # arrives 50 ms after tick() returned.
    got, thread = q.get(timeout=10)
    assert got == frame, (got, frame)
    assert thread != main
# A raising callback is printed, and delivery goes on.
calls = []
def bad(data):
    calls.append(data.frame)
    raise ValueError('boom')
lidar.stop()
lidar.listen(bad)
world.tick(); world.tick(); world.tick()
deadline = time.time() + 10
# One thread: the 3rd call follows the first two tracebacks.
while len(calls) < 3 and time.time() < deadline:
    time.sleep(0.01)
assert len(calls) == 3, calls
lidar.stop()
lidar.destroy()
print('OK')
"""


def test_callbacks_run_on_a_background_thread(pycarla):
    stderr = _run_ok(pycarla, _BLOCKING_QUEUE)
    assert stderr.count("ValueError: boom") >= 2, stderr


_SETUP = """
import ctypes, queue, threading, time
ctypes.CDLL(os.environ['TYPESAFE_CARLA_LIB']).tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(%d))
world = carla.Client('localhost', 2000).get_world()
lib = world.get_blueprint_library()
vehicle = world.get_actors()[0]
main = threading.get_ident()
def sensor(type_id='sensor.other.gnss'):
    return world.spawn_actor(lib.find(type_id), carla.Transform(), attach_to=vehicle)
"""


def test_import_starts_no_thread(pycarla):
    """The dispatcher starts at the first callback registration, not at import."""
    _run_ok(pycarla, _SETUP % 0 + """
assert threading.active_count() == 1, threading.enumerate()
g = sensor()
g.listen(lambda data: None)
assert [t.name for t in threading.enumerate()] == ['MainThread', 'carla-callbacks']
g.stop(); g.destroy()
print('OK')
""")


def test_on_tick_runs_on_the_background_thread(pycarla):
    _run_ok(pycarla, _SETUP % 50 + """
q = queue.Queue()
world.on_tick(lambda snapshot: q.put((snapshot.frame, threading.get_ident())))
for _ in range(3):
    frame = world.tick()
    got, thread = q.get(timeout=10)
    while got < frame:  # (snapshots of earlier frames may still be queued)
        got, thread = q.get(timeout=10)
    assert got == frame and thread != main, (got, frame)
print('OK')
""")


def test_no_callback_after_stop_returns(pycarla):
    _run_ok(pycarla, _SETUP % 20 + """
calls = []
g = sensor()
g.listen(lambda data: calls.append(data.frame))
for _ in range(5):
    world.tick()
time.sleep(0.2)
g.stop()
stopped = len(calls)
assert stopped > 0
for _ in range(5):
    world.tick()  # data keeps arriving for the stopped stream
time.sleep(0.5)
assert len(calls) == stopped, (stopped, calls)
g.destroy()
print('OK')
""")


def test_prompt_exit_with_a_listener(pycarla):
    """The dispatcher is a daemon thread, stopped at exit: the process ends at
    once, without the 1 s wait timeout or a hang."""
    started = time.monotonic()
    _run_ok(pycarla, _SETUP % 0 + """
g = sensor()
g.listen(lambda data: None)
world.tick()
print('OK')
""", timeout=30)
    assert time.monotonic() - started < 5


def test_forked_child_gets_callbacks(pycarla):
    """A forked child has no dispatcher thread: it starts its own, and both
    processes keep receiving callbacks."""
    _run_ok(pycarla, _SETUP % 0 + """
q = queue.Queue()
g = sensor()
g.listen(lambda data: q.put(data.frame))
world.tick()
q.get(timeout=10)
pid = os.fork()
if pid == 0:
    ok = False
    try:
        for _ in range(20):  # (Codon allocates on both threads of the child)
            frame = world.tick()
            got = q.get(timeout=10)
            while got < frame:
                got = q.get(timeout=10)
        ok = True
    finally:
        os._exit(0 if ok else 3)
_, status = os.waitpid(pid, 0)
assert os.WEXITSTATUS(status) == 0, status
frame = world.tick()
got = q.get(timeout=10)
while got < frame:
    got = q.get(timeout=10)
print('OK')
""")


def test_v2x_get_returns_the_python_api_dicts(pycarla):
    """Issue #101: get() of a custom message and of a CAM crashed under pycarla."""
    _run_ok(pycarla, _SETUP % 0 + """
other = world.spawn_actor(lib.find('vehicle.audi.tt'), carla.Transform(carla.Location(12.0, 0.0, 0.5)))
q = queue.Queue()
# Custom messages (smoke/test_v2x.py's path).
sender, receiver = sensor('sensor.other.v2x_custom'), world.spawn_actor(
    lib.find('sensor.other.v2x_custom'), carla.Transform(), attach_to=other)
receiver.listen(lambda event: q.put([m.get() for m in event]))
sender.send('hello v2x')
world.tick()
got = q.get(timeout=10)
p = got[0]['Message']['Message']
assert p == {'DataSize': 9, 'MaxDataSize': 100, 'Bytes': b'hello v2x'}, p
header = got[0]['Message']['Header']
assert header == {'Protocol Version': 2, 'Message ID': 'CUSTOM', 'Station ID': vehicle.id}, header
assert got[0]['Power'] == 21.5, got[0]['Power']
receiver.stop()
# A CAM: the nested dict, display strings included.
bp = lib.find('sensor.other.v2x')
bp.set_attribute('fixed_rate', 'true')
tx = world.spawn_actor(bp, carla.Transform(), attach_to=vehicle)
rx = world.spawn_actor(bp, carla.Transform(), attach_to=other)
rx.listen(lambda event: q.put([m.get() for m in event]))
world.tick()
cam = q.get(timeout=10)[0]
assert cam['Message']['Header']['Message ID'] == 'CAM', cam
basic = cam['Message']['Message']['CAM Parameters']['Basic Container']
assert basic['Station Type'] == 'Passenger Car', basic
assert set(basic['Reference Position']) == {'Latitude', 'Longitude', 'Position Confidence Eliipse'}
rx.stop()
print('OK')
""")


def test_raw_data_views_the_measurement_in_place(pycarla):
    """raw_data is a read-only view of the measurement's own bytes, as CARLA's
    memoryview is. It used to be copied through a Python list of ints
    (`tolist()`), which took ~0.3 s for a 128-channel LiDAR sweep: most of a
    closed loop's step."""
    _run_ok(pycarla, _SETUP % 0 + """
import gc
q = queue.Queue()
camera = sensor('sensor.camera.rgb')
camera.listen(q.put)
world.tick()
image = q.get(timeout=10)
raw = image.raw_data
assert isinstance(raw, memoryview) and raw.readonly and raw.format == 'B', raw
assert len(raw) == 4 * image.width * image.height, (len(raw), image.width, image.height)
# The bytes are the pixels', BGRA.
for i in (0, 1, len(image) - 1):
    c = image[i]
    assert tuple(raw[4 * i:4 * i + 4]) == (c.b, c.g, c.r, c.a), (i, tuple(raw[4 * i:4 * i + 4]), c)
try:
    raw[0] = 1
except TypeError:
    pass
else:
    raise AssertionError('raw_data is writable')
# The view keeps the measurement alive.
copy = bytes(raw)
del image
gc.collect()
world.tick()
assert bytes(raw) == copy
camera.stop(); camera.destroy()
print('OK')
""")


def test_callbacks_release_what_they_were_given(pycarla):
    """A measurement handed to a callback is released once Python drops it.
    Codon's pyobj.__call__ kept the argument tuple, so every frame a callback
    had received stayed alive with its native handle (8 MB a 1920x1080 image);
    and Boehm, which sees none of that native memory, never collected the rest.
    Up to two collection intervals (_callbacks._COLLECT_EVERY) may be pending."""
    _run_ok(pycarla, _SETUP % 0 + """
import gc
live = ctypes.CDLL(os.environ['TYPESAFE_CARLA_LIB']).tsc_live_handle_count
live.restype = ctypes.c_uint64
frames = []
kept = []  # what a callback keeps stays usable across collections
def on_image(image):
    frames.append(image.frame)
    if len(kept) < 3:
        kept.append(image)
camera = sensor('sensor.camera.rgb')
camera.listen(on_image)
for _ in range(5):
    world.tick()
gc.collect()
before = live()
for _ in range(200):
    world.tick()
deadline = time.time() + 20
while len(frames) < 205 and time.time() < deadline:
    time.sleep(0.05)
gc.collect()
after = live()
assert len(frames) >= 205, len(frames)
assert after - before < 32, (before, after)
assert [k.frame for k in kept] == frames[:3] and all(k.width > 0 and len(k.raw_data) for k in kept)
camera.stop(); camera.destroy()
print('OK')
""")


def test_returned_dicts_release_their_keys_and_values(pycarla):
    """A dict the library returns owns its keys and values alone. Codon's
    Dict.__to_py__ (PyDict_SetItem takes its own references) never released
    the ones its conversions made, so every attributes read (dozens of
    strings, which ACS reads every tick) stayed in memory for good."""
    _run_ok(pycarla, _SETUP % 0 + """
import sys
vehicle = world.spawn_actor(lib.find('vehicle.lincoln.mkz_2020'), carla.Transform(carla.Location(x=30)))
attrs = vehicle.attributes
assert attrs and all(isinstance(k, str) and isinstance(v, str) for k, v in attrs.items()), attrs
# (Not a one-character value: CPython's are shared, immortal.)
value = next(v for v in attrs.values() if len(v) > 1)
# Held by attrs and by `value` alone, as a value of a dict made here.
control = {'k': ''.join(['con', 'trol'])}
c = control['k']
assert sys.getrefcount(value) == sys.getrefcount(c), (sys.getrefcount(value), sys.getrefcount(c))
del attrs, value, control, c
# (On Python 3.10 the first 2000 or so reads take a block each and the rest
# none, with or without the fix: warm up past them.)
for _ in range(3000):
    vehicle.attributes
before = sys.getallocatedblocks()
for _ in range(2000):
    vehicle.attributes
after = sys.getallocatedblocks()
assert after - before < 200, (before, after)
print('OK')
""")


def test_uint64_values_are_unsigned(pycarla):
    """LibCarla's uint64 values (an episode id, an environment object's id)
    are unsigned, as CARLA's Python API gives them. typesafe_carla carries
    them bit-for-bit in a signed int: half of all episode ids read as
    negative, and a client took them for unknown. The mock's ids, as CARLA's,
    spread over all 64 bits: its first episode's is above 2**63."""
    _run_ok(pycarla, _SETUP % 0 + """
client = carla.Client('localhost', 2000)
first = world.id
assert first >= 2 ** 63 and first < 2 ** 64, first
assert world.get_snapshot().id == first, (world.get_snapshot().id, first)
assert str(world) == 'World(id=%d)' % first, str(world)
assert 0 <= world.get_map().get_waypoint(vehicle.get_location()).id < 2 ** 64
second = client.load_world('Town10HD_Opt').id
assert 0 <= second < 2 ** 63 and client.get_world().id == second, (first, second)

world = client.get_world()
building = world.get_environment_objects(carla.CityObjectLabel.Buildings)[0]
big = building.id
assert big >= 2 ** 63 and big < 2 ** 64, big
# Taken back, in a list or a set as CARLA's Python API takes them.
world.enable_environment_objects([big], False)
world.enable_environment_objects({big}, True)
o = carla.EnvironmentObject(big, 'x', building.transform, building.bounding_box,
                            carla.CityObjectLabel.Buildings)
assert o.id == big, o.id
o.id = 2 ** 64 - 1
assert o.id == 2 ** 64 - 1, o.id
o.id = 7
assert o.id == 7, o.id
print('OK')
""")


def test_instances_take_arbitrary_attributes(pycarla):
    """As Boost.Python's: upstream tests set UE4-era fields (e.g.
    WheelPhysicsControl.tire_friction) and read them back."""
    _run_ok(pycarla, """
wheel = carla.WheelPhysicsControl()
wheel.tire_friction = 3.0
assert wheel.tire_friction == 3.0
loc = carla.Location(1, 2, 3)
loc.note = 'x'
assert loc.note == 'x' and loc.x == 1
loc.x = 5
assert loc.x == 5 and 'x' not in vars(loc)  # a field still goes to the library
print('OK')
""")


def test_physics_controls_ignore_unknown_keywords(pycarla):
    """#93: as in CARLA's Python API (raw-kwargs constructors), the physics
    controls drop keywords they do not know, here with a warning; every other
    constructor rejects them with a TypeError, as Boost.Python's ArgumentError."""
    _run_ok(pycarla, """
import warnings
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter('always')
    wheel = carla.WheelPhysicsControl(tire_friction=2, max_steer_angle=30, radius=10)
    physics = carla.VehiclePhysicsControl(moi=1, use_gear_autobox=1, mass=1000)
assert wheel.max_steer_angle == 30 and physics.mass == 1000
assert not hasattr(wheel, 'tire_friction') and not hasattr(physics, 'moi')
messages = [str(w.message) for w in caught]
assert len(messages) == 2 and "['radius', 'tire_friction']" in messages[0], messages
for make in (lambda: carla.VehicleControl(throttle=0.5, bogus=1), lambda: carla.Location(x=1, bogus=1)):
    try:
        make()
    except TypeError:
        pass
    else:
        raise AssertionError('an unknown keyword was accepted')
print('OK')
""")


def test_custom_v2x_set_string_keeps_nul(pycarla):
    """Issue #99: set_string copies the whole str, NULs included, as CARLA's
    Python API and as set_bytes; so does Sensor.send(str)."""
    _run_ok(pycarla, _SETUP % 0 + """
s, b = carla.CustomV2XBytes(), carla.CustomV2XBytes()
s.set_string('hi\\x00z')
b.set_bytes(b'hi\\x00z')
assert s.get() == b.get() == {'DataSize': 4, 'MaxDataSize': 100, 'Bytes': b'hi\\x00z'}, s.get()
assert s == b and s.data_size == 4 and s.get_string() == 'hi\\x00z'
s.set_string('\\x00a' * 60)  # cut to 100 bytes
b.set_bytes(b'\\x00a' * 50)
assert s == b and s.data_size == 100
other = world.spawn_actor(lib.find('vehicle.audi.tt'), carla.Transform(carla.Location(12.0, 0.0, 0.5)))
q = queue.Queue()
sender, receiver = sensor('sensor.other.v2x_custom'), world.spawn_actor(
    lib.find('sensor.other.v2x_custom'), carla.Transform(), attach_to=other)
receiver.listen(lambda event: q.put([m.get() for m in event]))
sender.send('hi\\x00z')
world.tick()
got = q.get(timeout=10)
assert got[0]['Message']['Message']['Bytes'] == b'hi\\x00z', got
receiver.stop()
print('OK')
""")


def test_str_arguments_keep_nul(launcher, tmp_path):
    """Issue #99, without the full package: the generated module's
    str.__from_py__ (pycarla.STR_FROM_PY) keeps a str's NULs, where Codon's
    own stops at the first; bytes, UTF-8 and encoding errors are unchanged."""
    import shutil
    import sys

    from typesafe_carla import pycarla

    if shutil.which("cc") is None:
        pytest.skip("needs cc to link the extension")
    source = tmp_path / "nul.codon"
    source.write_text("import internal.python as _ipy\n" + pycarla.STR_FROM_PY + """
def length(s: str) -> int:
    return len(s)

def echo(s: str) -> str:
    return s
""")
    obj = tmp_path / "nul.o"
    result = launcher("build", "--pyext", "--relocation-model=pic", "--module", "nul",
                      "-o", str(obj), str(source))
    assert result.returncode == 0, result.stdout + result.stderr
    pycarla.link(obj, tmp_path / "nul.so")
    program = r"""
import nul
assert nul.length('hi\x00z') == 4 and nul.echo('hi\x00z') == 'hi\x00z'
assert nul.length(b'hi\x00z') == 4 and nul.length('\u00e9') == 2
assert nul.length('') == 0 and nul.length('\x00') == 1
try:
    nul.length('\ud800')  # not UTF-8: refused, as by Codon's own conversion
    raise AssertionError('a lone surrogate was accepted')
except TypeError:
    pass
print('OK')
"""
    run = subprocess.run([sys.executable, "-c", program], cwd=tmp_path,
                         capture_output=True, text=True, timeout=60)
    assert run.returncode == 0 and run.stdout.split() == ["OK"], run.stdout + run.stderr


def test_actor_attribute_conversions(pycarla):
    """Issue #102: int() / float() / bool() as the Python API's __int__ /
    __float__ / __bool__ (As<int> / As<float> / As<bool>), raising
    RuntimeError (CARLA's BadAttributeCast) for another attribute type."""
    _run_ok(pycarla, """
bp = carla.Client('localhost', 2000).get_world().get_blueprint_library().find('vehicle.lincoln.mkz_2020')
def raises(f):
    try:
        f()
    except RuntimeError:
        return True
    return False
wheels = bp.get_attribute('number_of_wheels')
assert int(wheels) == 4 and type(int(wheels)) is int
assert raises(lambda: float(wheels)) and raises(lambda: bool(wheels))
mass = bp.get_attribute('base_mass')
assert float(mass) == 1500.0 and raises(lambda: int(mass))
sticky = bp.get_attribute('sticky_control')
assert bool(sticky) is True and raises(lambda: int(sticky))
assert raises(lambda: 1 if bp.get_attribute('role_name') else 0)  # truthiness is bool()
print('OK')
""")


# Issue #122: the blocking calls release the GIL while they wait in LibCarla.
# The mock's calls that wait for a server take DELAY ms more
# (tsc_mock_set_call_delay_ms); a probe thread counts how far it gets meanwhile,
# against how far it gets while the main thread sleeps.
_GIL_PROBE = """
import ctypes, queue, threading, time
mock = ctypes.CDLL(os.environ['TYPESAFE_CARLA_LIB'])
DELAY = 0.3
mock.tsc_mock_set_call_delay_ms(ctypes.c_uint32(int(DELAY * 1000)))
client = carla.Client('localhost', 2000)
world = client.get_world()
main = threading.get_ident()
count = [0]
stop = threading.Event()
def probe():
    while not stop.is_set():
        count[0] += 1
threading.Thread(target=probe, daemon=True).start()
time.sleep(0.05)
def progress(call):
    before, start = count[0], time.monotonic()
    try:
        call()
    finally:
        took = time.monotonic() - start
    assert took >= DELAY * 0.9, took  # the call did block
    return (count[0] - before) / took
free = progress(lambda: time.sleep(DELAY))
"""


def test_blocking_calls_release_the_gil(pycarla):
    _run_ok(pycarla, _GIL_PROBE + """
def raises(call, text):
    try:
        call()
    except RuntimeError as e:
        assert text in str(e), e
    else:
        raise AssertionError('no error')
rates = {
    'tick': progress(world.tick),
    'load_world': progress(lambda: client.load_world('Town10HD_Opt')),
    'reload_world': progress(client.reload_world),
    # Raising after the GIL is taken back (the mock refuses the map after the delay).
    'load_world raising': progress(lambda: raises(lambda: client.load_world('Nowhere'),
                                                  'not found')),
}
world = client.get_world()
settings = world.get_settings()
settings.synchronous_mode = True
world.apply_settings(settings)
rates['wait_for_tick raising'] = progress(lambda: raises(lambda: world.wait_for_tick(5.0),
                                                         'synchronous mode'))
slow = {k: round(v / free, 2) for k, v in rates.items() if v < 0.3 * free}
assert not slow, (free, slow)
print('OK')
""")


def test_blocking_calls_hold_the_gil_on_request(pycarla):
    """TYPESAFE_CARLA_HOLD_GIL=1: as before #122 (and the probe does measure it)."""
    _run_ok(pycarla, _GIL_PROBE + """
rate = progress(world.tick)
assert rate < 0.1 * free, (rate, free)
print('OK')
""", env={"TYPESAFE_CARLA_HOLD_GIL": "1"})


def test_callbacks_during_gil_free_ticks(pycarla):
    """Callbacks on the dispatch thread run while the main thread waits in
    tick() without the GIL (the mock delivers each tick's data 50 ms late, so
    during the next tick); at the dispatch points (no dispatch thread) they
    run inside tick(), on the main thread, once the GIL is taken back."""
    stderr = _run_ok(pycarla, _GIL_PROBE + """
mock.tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(50))
lib = world.get_blueprint_library()
vehicle = world.get_actors()[0]
def spawn(type_id):
    return world.spawn_actor(lib.find(type_id), carla.Transform(), attach_to=vehicle)
q = queue.Queue()
fail = []
def put(kind, frame):
    q.put((kind, frame, threading.get_ident()))
    if fail:
        raise ValueError('boom at a dispatch point')
camera = spawn('sensor.camera.rgb')
lidar = spawn('sensor.lidar.ray_cast')
camera.listen(lambda image: put('camera', image.frame + 0 * len(bytes(image.raw_data))))
lidar.listen(lambda data: put('lidar', data.frame))
world.on_tick(lambda snapshot: put('tick', snapshot.frame))
def expect(frames, on_main):
    want = {(k, f) for f in frames for k in ('camera', 'lidar', 'tick')}
    deadline = time.time() + 10
    while want:
        kind, frame, thread = q.get(timeout=max(deadline - time.time(), 0.01))
        if (kind, frame) in want:
            want.discard((kind, frame))
            assert (thread == main) == on_main, (kind, thread, main)
frames = []
for _ in range(5):
    rate = progress(lambda: frames.append(world.tick()))
    assert rate > 0.3 * free, (rate, free)
expect(frames, False)
# The dispatch points: no dispatch thread.
from carla import _runtime
assert _runtime._stop_dispatcher('the test stopped it')
carla.set_auto_dispatch(True)
mock.tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(0))
frames = []
for _ in range(3):
    rate = progress(lambda: frames.append(world.tick()))
    assert rate > 0.3 * free, (rate, free)
    expect(frames[-1:], True)
# A callback raising at a dispatch point is printed; tick() goes on.
fail.append(True)
for _ in range(2):
    frames.append(world.tick())
    expect(frames[-1:], True)
fail.clear()
camera.stop(); lidar.stop(); camera.destroy(); lidar.destroy()
print('OK')
""")
    assert "boom at a dispatch point" in stderr, stderr


def test_other_python_threads_may_call_in(pycarla):
    """A Python thread other than the importing one and the dispatcher calls
    into Codon: it is registered with Codon's collector for each call, so a
    collection it starts does not abort ("Collecting from unknown thread"),
    while the main thread ticks without the GIL."""
    _run_ok(pycarla, _GIL_PROBE + """
from carla import _runtime
mock.tsc_mock_set_call_delay_ms(ctypes.c_uint32(5))
lib = world.get_blueprint_library()
vehicle = world.get_actors()[0]
lidar = world.spawn_actor(lib.find('sensor.lidar.ray_cast'), carla.Transform(), attach_to=vehicle)
got = []
lidar.listen(lambda data: got.append(data.frame))
done, errors = [], []
def worker():
    try:
        for _ in range(3000):  # allocates enough for several collections
            world.get_blueprint_library().filter('vehicle.*')
            world.get_actors()
            vehicle.get_transform()
        done.append(True)
    except BaseException as e:
        errors.append(repr(e))
threads = [threading.Thread(target=worker) for _ in range(3)]
for t in threads:
    t.start()
while any(t.is_alive() for t in threads):
    world.tick()
for t in threads:
    t.join()
assert len(done) == 3 and not errors, errors
assert getattr(_runtime._thread, 'attach', None) is False  # the main thread: for good
assert got
lidar.stop(); lidar.destroy()
# Collections on the worker itself: a polled measurement is delivered there,
# and the library collects every 16 deliveries on a registered thread. The
# main thread only waits meanwhile (no Codon code).
gnss = world.spawn_actor(lib.find('sensor.other.gnss'), carla.Transform(), attach_to=vehicle)
gnss.listen(None, 0)  # polling mode, unbounded
mock.tsc_mock_set_call_delay_ms(ctypes.c_uint32(0))
for _ in range(64):
    world.tick()
gc_no = ctypes.PyDLL(_runtime._ext.__file__).GC_get_gc_no
gc_no.restype = ctypes.c_size_t
polled = []
def poller():
    try:
        while gnss.poll() is not None:
            polled.append(1)
    except BaseException as e:
        errors.append(repr(e))
before = gc_no()
t = threading.Thread(target=poller)
t.start(); t.join()
assert not errors and len(polled) >= 64, (errors, len(polled))
assert gc_no() >= before + 3, (before, gc_no())  # 64 deliveries: 4 collections there
gnss.stop(); gnss.destroy()
print('OK')
""", timeout=600)


def test_attach_current_thread_under_pycarla(pycarla):
    """carla.attach_current_thread() / detach_current_thread() on a Python
    thread: registered until detach (its calls skip the per-call
    registration), then per call again; neither crashes nor undoes the
    other's registration."""
    _run_ok(pycarla, _GIL_PROBE + """
from carla import _runtime
registered = ctypes.PyDLL(_runtime._ext.__file__).GC_thread_is_registered
assert carla.attach_current_thread() is False  # the main thread already is
errors = []
def worker():
    try:
        assert not registered()
        carla.detach_current_thread()  # not registered: nothing to undo
        assert carla.attach_current_thread() is True
        assert carla.attach_current_thread() is False
        for _ in range(2000):
            world.get_blueprint_library().filter('vehicle.*')
        assert registered()  # the calls did not unregister it
        carla.detach_current_thread()
        assert not registered()
        for _ in range(2000):  # per call again
            world.get_blueprint_library().filter('vehicle.*')
        assert not registered()
    except BaseException as e:
        import traceback
        errors.append(traceback.format_exc())
threads = [threading.Thread(target=worker) for _ in range(3)]
for t in threads:
    t.start()
for t in threads:
    t.join()
assert not errors, errors
print('OK')
""")


def test_remove_on_tick_during_a_gil_free_wait(pycarla):
    """At the dispatch points, tick() waits (GIL released) for the on_tick
    snapshot; another thread removing that callback meanwhile must not free
    the listener under the wait (issue #122 review)."""
    _run_ok(pycarla, _GIL_PROBE + """
from carla import _runtime
mock.tsc_mock_set_call_delay_ms(ctypes.c_uint32(0))
ids = [world.on_tick(lambda s: None) for _ in range(4)]
assert _runtime._stop_dispatcher('the test stopped it')
carla.set_auto_dispatch(True)
mock.tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(300))  # tick() then waits for them
live = mock.tsc_live_handle_count
live.restype = ctypes.c_uint64
kept = []
def remove(callback_id):
    # tick() is waiting on this callback's listener (the first one left).
    before = live()
    world.remove_on_tick(callback_id)
    kept.append(live() == before)  # the wait's own reference keeps it
for callback_id in ids:
    remover = threading.Timer(0.1, remove, (callback_id,))
    remover.start()
    start = time.monotonic()
    world.tick(1.0)  # (a removed callback's frame never comes: waits out the 1 s)
    remover.join()
    assert time.monotonic() - start < 3
assert all(kept), kept
mock.tsc_mock_set_delivery_delay_ms(ctypes.c_uint32(0))
world.tick()
print('OK')
""")
