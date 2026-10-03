"""Ports CARLA's map-dependent PythonAPI tests to maps shipped in ue5-dev (#80).

About 55 of CARLA's own tests load Town03, Town05(_Opt) or Town01, which no
ue5-dev package ships; the official module fails them the same way. This
tool writes ported copies to tests/upstream/ported/, from the upstream tests
at PORT_COMMIT, by mechanical rules (RULES below). Each copy's header records
its provenance and what changed. The originals get `exclude:` entries in
tests/upstream/expectations.yaml, and the copies run as the `ported` suite.

The changes, and why:
- `load_world("Town03")` and friends become `load_world(shipped_map(client))`:
  Town10HD_Opt, or Town15 / Mine_01 where a test needs a larger map
  (SkipTest if the server has none of them).
- SmokeTest.tearDown, which reloads Town03, does the same (the ported
  smoke/__init__.py). Smoke tests that fail only through it are copied
  unchanged, so that `from . import SmokeTest` picks the ported base.
- Coordinates written for a straight road of Town03 / Town05 / Town01 go
  through a MapFrame. It maps the upstream road's start and heading onto a
  straight driving lane of the loaded map, keeping distances and angles, and
  maps readings (`.get_location().y`, `.get_velocity().y`, yaw) back into
  the upstream frame, so the assertions are unchanged.

Every ported test must pass with the official module on a ue5-dev server
(`python -m tools.upstream_tests --mode official --suite ported`).

    python -m tools.port_upstream_tests [--tests-dir PythonAPI/test]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "upstream" / "ported"
# The ue5-dev commit the ported copies are made from (the test server's).
PORT_COMMIT = "0a5ce0d5b4952bd8294a163c12d49f197bdb2aba"

HELPERS = '''
# ---- ported helpers (tools/port_upstream_tests.py, issue #80) --------------
import math as _math
import unittest as _unittest

# Maps shipped in ue5-dev packages (local build: Town10HD_Opt, RoadgenCross;
# official nightly: EmptyMap, Mine_01, OpenDriveMap, Town10HD_Opt, Town15).
SHIPPED_MAPS = ("Town10HD_Opt", "Town15", "Mine_01")
LARGE_MAPS = ("Town15", "Mine_01", "Town10HD_Opt")


def shipped_map(client, prefer=SHIPPED_MAPS):
    """The first of `prefer` the server has (SkipTest if none)."""
    available = [m.split("/")[-1] for m in client.get_available_maps()]
    for name in prefer:
        if name in available:
            return name
    raise _unittest.SkipTest("none of %s on this server (it has %s)"
                             % (", ".join(prefer), ", ".join(available)))


class MapFrame:
    """An upstream test's road, placed on a straight road of the loaded map.

    `origin` and `heading` are a point and direction on the upstream map's
    straight road (where the test spawns its vehicles); they map to the start
    and direction of the longest straight driving lane of the loaded map, up
    to `length` m. Distances and angles are kept. up_location / up_vector /
    up_yaw map the loaded map's readings back into upstream coordinates, so
    upstream assertions on `.x`, `.y` and yaw still hold.
    """

    def __init__(self, world, origin, heading, length):
        self.origin = origin
        wp, self.run = self._straight(world.get_map(), length)
        transform = wp.transform
        self.base = transform.location
        self.dyaw = transform.rotation.yaw - heading
        self.theta = _math.radians(self.dyaw)

    @staticmethod
    def _straight(m, length, step=5.0):
        best = None
        for wp in m.generate_waypoints(step):
            if wp.is_junction or wp.lane_type != carla.LaneType.Driving:
                continue
            yaw0 = wp.transform.rotation.yaw
            run, cur = 0.0, wp
            while run < length:
                nxt = cur.next(step)
                if not nxt or nxt[0].is_junction:
                    break
                if abs((nxt[0].transform.rotation.yaw - yaw0 + 180.0) % 360.0 - 180.0) > 1.0:
                    break
                run += step
                cur = nxt[0]
            if best is None or run > best[1]:
                best = (wp, run)
            if run >= length:
                break
        return best

    def _rot(self, dx, dy, sign=1.0):
        c, s = _math.cos(sign * self.theta), _math.sin(sign * self.theta)
        return c * dx - s * dy, s * dx + c * dy

    def location(self, x, y, z=0.0):
        dx, dy = self._rot(x - self.origin[0], y - self.origin[1])
        return carla.Location(self.base.x + dx, self.base.y + dy, self.base.z + z)

    def transform(self, x, y, z=0.0, yaw=0.0, pitch=0.0, roll=0.0):
        return carla.Transform(self.location(x, y, z),
                               carla.Rotation(pitch=pitch, yaw=yaw + self.dyaw, roll=roll))

    def vector(self, x, y, z=0.0):
        dx, dy = self._rot(x, y)
        return carla.Vector3D(dx, dy, z)

    def up_location(self, loc):
        dx, dy = self._rot(loc.x - self.base.x, loc.y - self.base.y, -1.0)
        return carla.Location(self.origin[0] + dx, self.origin[1] + dy, loc.z - self.base.z)

    def up_vector(self, v):
        dx, dy = self._rot(v.x, v.y, -1.0)
        return carla.Vector3D(dx, dy, v.z)

    def up_yaw(self, yaw):
        return (yaw - self.dyaw + 180.0) % 360.0 - 180.0
# ---- end of ported helpers --------------------------------------------------
'''

TRANSFORM = re.compile(
    r"carla\.Transform\(carla\.Location\(([^()]*)\),\s*carla\.Rotation\(([^()]*)\)\)")
TRANSFORM_NO_ROT = re.compile(r"carla\.Transform\(carla\.Location\(([^()]*)\)\)")


def framed(text: str, frame: str, vectors: bool = True) -> str:
    """Upstream world coordinates -> `frame` (transforms, velocities, readings)."""
    text = TRANSFORM.sub(lambda m: f"{frame}.transform({m[1]}, {m[2]})", text)
    text = TRANSFORM_NO_ROT.sub(lambda m: f"{frame}.transform({m[1]})", text)
    if vectors:
        text = re.sub(r"carla\.Vector3D\(", f"{frame}.vector(", text)
    text = re.sub(r"([\w\[\]\.]+)\.get_velocity\(\)\.([xy])\b", rf"{frame}.up_vector(\1.get_velocity()).\2", text)
    text = re.sub(r"([\w\[\]\.]+)\.get_location\(\)\.([xy])\b", rf"{frame}.up_location(\1.get_location()).\2", text)
    return text


def replace(text: str, old: str, new: str, count: int = -1) -> str:
    if old not in text:
        raise SystemExit(f"port rule: {old!r} not found (upstream changed?)")
    return text.replace(old, new, count)


# --- rules: upstream path -> (function(text) -> text, [changes]) -------------

def _smoke_init(t: str):
    t = replace(t, 'self.client.load_world("Town03")', "self.client.load_world(shipped_map(self.client))")
    return t + HELPERS, ["tearDown loads a shipped map (Town10HD_Opt) instead of Town03",
                         "adds shipped_map() and MapFrame for the ported tests"]


def _copy(t: str):
    return t, ["none: copied so that `from . import SmokeTest` uses the ported base, whose "
               "tearDown loads a shipped map instead of Town03"]


def _determinism(t: str):
    t = replace(t, "from . import SmokeTest", "from . import SmokeTest, shipped_map")
    t = replace(t, 'self.client.load_world("Town03")', "self.client.load_world(shipped_map(self.client))")
    return t, ["loads a shipped map instead of Town03 (the test uses the map's own spawn points)"]


def _collision_determinism(t: str):
    t = replace(t, "from . import SmokeTest", "from . import SmokeTest, MapFrame, shipped_map")
    t = replace(t, 'self.client.load_world("Town03")', "self.client.load_world(shipped_map(self.client))")
    # Scenarios: Town03's straight road along +x at y = -255, from x = 40.
    head, sep, rest = t.partition("class TwoCarsHighSpeedCollision(Scenario):")
    tail_mark = "class CollisionScenarioTester"
    scen, sep2, tail = rest.partition(tail_mark)
    scen = re.sub(r"(        super\(\w+, self\)\.init_scene\(prefix, settings, spectator_tr\)\n)",
                  r"\1        frame = MapFrame(self.world, (40, -255), 0.0, 110.0)\n", scen)
    scen = framed(scen, "frame")
    return head + sep + scen + sep2 + tail, [
        "loads a shipped map instead of Town03",
        "the scenarios' spawn transforms and target velocities, written for Town03's straight "
        "road at y = -255 (from x = 40, heading +x), go through a MapFrame onto a straight "
        "road of the loaded map; the side-road vehicles keep their offsets from that road"]


def _vehicle_physics(t: str):
    t = replace(t, "from . import SyncSmokeTest", "from . import SyncSmokeTest, MapFrame, shipped_map, LARGE_MAPS")
    # Town05_Opt: a straight road along +y at x = 31, from y = -200.
    t = replace(t, '''        self.client.load_world("Town05_Opt", False)
        # workaround: give time to UE4 to clean memory after loading (old assets)
        time.sleep(5)
''', '''        self.client.load_world(shipped_map(self.client, LARGE_MAPS), False)
        # workaround: give time to UE4 to clean memory after loading (old assets)
        time.sleep(5)
        self.world = self.client.get_world()
        self.frame = MapFrame(self.world, (31, -200), 90.0, 250.0)
''')
    head, sep, rest = t.partition("class TestVehicleFriction(SyncSmokeTest):")
    rest = framed(rest, "self.frame")
    # TestStickyControl: Town05's road along +y at x = 235, from y = -1.
    rest = replace(rest, '''        ref_pos = -1
        veh_transf = self.frame.transform(''', '''        ref_pos = -1
        self.frame = MapFrame(self.world, (235, -1), 90.0, 250.0)
        veh_transf = self.frame.transform(''')
    return head + sep + rest, [
        "loads a shipped large map (Town15, else Mine_01 or Town10HD_Opt) instead of Town05_Opt",
        "spawn transforms, target velocities and the friction volume, written for Town05's "
        "straight road along +y (x = 31 and x = 235), go through a MapFrame onto the longest "
        "straight road of the loaded map; readings of .y go back through it, so the "
        "assertions are unchanged"]


def _api_collision(t: str):
    t = replace(t, "world = client.load_world('Town01')",
                "world = client.load_world(shipped_map(client))\n"
                "        frame = MapFrame(world, (177.7, 198.8), 0.0, 40.0)")
    t = re.sub(r"(walker = world\.spawn_actor\(bp, )carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(\)\)\)",
               r"\1frame.transform(\2))", t)
    t = re.sub(r"(vehicle = world\.spawn_actor\(bp, )carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(\)\)\)",
               r"\1frame.transform(\2))", t)
    t = replace(t, "import carla\n", "import carla\n" + HELPERS, 1)
    return t, ["loads a shipped map instead of Town01",
               "the walker and the vehicle behind it, on Town01's road at y = 199 heading +x, go "
               "through a MapFrame onto a straight road of the loaded map"]


def _top_vehicle_physics(t: str):
    t = replace(t, '''        if world.get_map().name != "Town05":
            client.load_world("Town05", False)
''', '''        if world.get_map().name.split("/")[-1] != shipped_map(client, LARGE_MAPS):
            client.load_world(shipped_map(client, LARGE_MAPS), False)
        world = client.get_world()
        global FRAME
        FRAME = MapFrame(world, (32, -180), 90.0, 250.0)
''')
    # Spawn points and stop conditions in Town05 coordinates.
    t = re.sub(r"(init_(?:loc|pos) = )carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(([^()]*)\)\)",
               r"\1FRAME.transform(\2, \3)", t)
    t = replace(t, "        loc = vehicle.get_location()\n", "        loc = FRAME.up_location(vehicle.get_location())\n")
    t = replace(t, "        rot = vehicle.get_transform().rotation\n",
                "        rot = vehicle.get_transform().rotation\n        rot.yaw = FRAME.up_yaw(rot.yaw)\n")
    t = replace(t, "import carla\n", "import carla\n" + HELPERS + "\nFRAME = None\n", 1)
    return t, ["loads a shipped large map (Town15, else Mine_01 or Town10HD_Opt) instead of Town05",
               "the scenarios' start transforms and the stop conditions on location and yaw, "
               "written for Town05 (x = 32, heading +y), go through a MapFrame"]


# Smoke tests that fail only through SmokeTest.tearDown (Town03).
BASE_ONLY = ["test_blueprint", "test_client", "test_collision_sensor", "test_geoconversion",
             "test_lidar", "test_map", "test_props_loading", "test_sensor_determinism",
             "test_sensor_tick_time", "test_snapshot", "test_spawnpoints", "test_streamming",
             "test_sync", "test_world"]

RULES = {
    "smoke/__init__.py": _smoke_init,
    **{f"smoke/{n}.py": _copy for n in BASE_ONLY},
    "smoke/test_determinism.py": _determinism,
    "smoke/test_collision_determinism.py": _collision_determinism,
    "smoke/test_vehicle_physics.py": _vehicle_physics,
    "API/test_collision.py": _api_collision,
    "test_vehicle_physics.py": _top_vehicle_physics,
}


# The map each original needs (for its `exclude:` entry).
NEEDED_MAP = {
    **{f"smoke/{n}.py": "Town03" for n in BASE_ONLY},
    "smoke/test_determinism.py": "Town03",
    "smoke/test_collision_determinism.py": "Town03",
    "smoke/test_vehicle_physics.py": "Town05_Opt",
    "API/test_collision.py": "Town01",
    "test_vehicle_physics.py": "Town05",
}


def exclude_originals(refs=("ue5-dev",)) -> None:
    """Marks the ported originals `exclude:` in tests/upstream/expectations.yaml."""
    from tools import upstream_tests as up

    for mode in up.MODES:
        manifest = up.load_manifest(mode)
        for ref in refs:
            entries = manifest.setdefault(ref, {})
            for rel, name in NEEDED_MAP.items():
                entries[rel] = (f"exclude: map {name} not shipped in ue5-dev packages; "
                                f"ported to tests/upstream/ported/{rel}")
        up.write_manifest(mode, manifest)


def header(rel: str, changes: list[str]) -> str:
    lines = ["# Ported from CARLA's PythonAPI/test/" + rel + " at carla-simulator/carla@" + PORT_COMMIT,
             "# by tools/port_upstream_tests.py (typesafe_carla issue #80): the original needs a map",
             "# (Town03 / Town05 / Town01) that no ue5-dev package ships. Do not edit by hand:",
             "# change the rules there and regenerate. Changes:"]
    lines += [f"#   - {c}" for c in changes]
    return "\n".join(lines) + "\n#\n"


def port(tests: Path, out: Path = OUT) -> list[str]:
    written = []
    for rel, rule in RULES.items():
        text = (tests / rel).read_text()
        new, changes = rule(text)
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(header(rel, changes) + new)
        written.append(rel)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m tools.port_upstream_tests",
                                     description=__doc__.split("\n\n")[0])
    parser.add_argument("--tests-dir", type=Path,
                        help=f"PythonAPI/test at {PORT_COMMIT[:10]} (default: fetched)")
    parser.add_argument("--exclude-originals", action="store_true",
                        help="also mark the originals `exclude:` in tests/upstream/expectations.yaml (ue5-dev)")
    args = parser.parse_args(argv)
    tests = args.tests_dir
    if tests is None:
        from tools import upstream_tests

        tests = upstream_tests.fetch_tests(PORT_COMMIT)
    for rel in port(tests):
        print(f"ported {rel}")
    if args.exclude_originals:
        exclude_originals()
        print("marked the originals `exclude:` in tests/upstream/expectations.yaml")
    return 0


if __name__ == "__main__":
    sys.exit(main())
