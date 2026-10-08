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


def _run_ok(pydir, program: str, timeout: float = 120) -> str:
    """Runs `program` with the generated `carla`; checks it printed only OK and
    returns its stderr."""
    # As the upstream-test driver: load the generated package explicitly (an
    # installed official `carla` must not win).
    head = (
        "import importlib.util, os, sys\n"
        "pkg = os.environ['TSC_PYCARLA_PKG']\n"
        "spec = importlib.util.spec_from_file_location('carla', os.path.join(pkg, '__init__.py'),"
        " submodule_search_locations=[pkg])\n"
        "carla = importlib.util.module_from_spec(spec); sys.modules['carla'] = carla\n"
        "spec.loader.exec_module(carla)\n")
    result = subprocess.run([up._python(), "-c", head + program], env=up._cpython_env(pydir, False),
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
