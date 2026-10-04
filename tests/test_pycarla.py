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
