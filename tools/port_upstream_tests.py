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
# Maps the ue5-dev server lists but cannot load, with the official module too
# (RoadgenCross: "unable to parse the OpenDRIVE XML string" in the server log).
UNLOADABLE_MAPS = ("RoadgenCross",)


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

    def __init__(self, world, origin, heading, length, side=None, _lane=None):
        self.origin = origin
        wp, self.run = _lane or self._straight(world.get_map(), length, side)
        self._lane = (wp, self.run)
        transform = wp.transform
        self.base = transform.location
        self.lane_yaw = transform.rotation.yaw
        self.dyaw = transform.rotation.yaw - heading
        self.theta = _math.radians(self.dyaw)

    def at(self, world, origin, heading):
        """Another upstream point and direction on this frame's lane (no new
        search): a scenario that starts elsewhere on the upstream map starts
        at the lane's start too."""
        return MapFrame(world, origin, heading, self.run, _lane=self._lane)

    def spawn(self, world, blueprint, transform, move=True, tries=8, step=5.0):
        """world.spawn_actor, robust to the loaded map: the upstream z values
        assume flat UE4 ground, so a blocked spawn is retried 1 m higher, then
        (with `move`) 5 m further along the lane, moving the whole frame with
        it so later transforms and readings keep their relation. The last
        attempt raises the server's own error."""
        for i in range(tries):
            for dz in (0.0, 1.0):
                t = carla.Transform(carla.Location(transform.location.x, transform.location.y,
                                                   transform.location.z + dz), transform.rotation)
                if i == tries - 1 and dz:
                    return world.spawn_actor(blueprint, t)
                actor = world.try_spawn_actor(blueprint, t)
                if actor is not None:
                    return actor
            if not move:
                return world.spawn_actor(blueprint, transform)
            dx = step * _math.cos(_math.radians(self.lane_yaw))
            dy = step * _math.sin(_math.radians(self.lane_yaw))
            self.base = carla.Location(self.base.x + dx, self.base.y + dy, self.base.z)
            transform = carla.Transform(carla.Location(transform.location.x + dx,
                                                       transform.location.y + dy,
                                                       transform.location.z), transform.rotation)

    @staticmethod
    def _beside(wp, side):
        """A same-direction driving lane on `side` ("left"/"right") of wp."""
        other = wp.get_left_lane() if side == "left" else wp.get_right_lane()
        return (other is not None and other.lane_type == carla.LaneType.Driving
                and (other.lane_id > 0) == (wp.lane_id > 0))

    @staticmethod
    def _straight(m, length, side=None, step=5.0):
        best = None
        for wp in m.generate_waypoints(step):
            if wp.is_junction or wp.lane_type != carla.LaneType.Driving:
                continue
            if side and not MapFrame._beside(wp, side):
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
        if best is None and side:  # no such lane anywhere: any straight road
            return MapFrame._straight(m, length, None, step)
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
    t = replace(t, "'vehicle.carlamotors.firetruck']",
                "'vehicle.carlamotors.firetruck',\n"
                "    # port: ue5-dev's trucks and buses (Docs/catalogue_vehicles.md base types)\n"
                "    'vehicle.carlacola.actors', 'vehicle.firetruck.actors', 'vehicle.fuso.mitsubishi',\n"
                "    'vehicle.miningtruck.miningtruck']")
    return t + HELPERS, ["tearDown loads a shipped map (Town10HD_Opt) instead of Town03",
                         "the large-vehicle exclusion list (UE4 ids only, so stale on ue5-dev) also "
                         "names ue5-dev's trucks and buses (base_type truck / bus in "
                         "Docs/catalogue_vehicles.md at the same commit): vehicle.carlacola.actors, "
                         "vehicle.firetruck.actors, vehicle.fuso.mitsubishi and "
                         "vehicle.miningtruck.miningtruck",
                         "adds shipped_map() and MapFrame for the ported tests"]


def _copy(t: str):
    return t, ["none: copied so that `from . import SmokeTest` uses the ported base, whose "
               "tearDown loads a shipped map instead of Town03"]


def _map(t: str):
    t = replace(t, "and map_name != '/Game/Carla/Maps/Town12/Town12':",
                "and map_name != '/Game/Carla/Maps/Town12/Town12' \\\n"
                "                    and map_name.split('/')[-1] not in UNLOADABLE_MAPS:")
    t = replace(t, "from . import SmokeTest", "from . import SmokeTest, UNLOADABLE_MAPS")
    return t, ["test_load_all_maps also skips the maps in UNLOADABLE_MAPS (RoadgenCross), which "
               "the ue5-dev server lists but cannot load, with the official module too: its "
               "OpenDRIVE does not parse",
               "otherwise copied so that `from . import SmokeTest` uses the ported base"]


def _spawnpoints(t: str):
    t = replace(t, "from . import SyncSmokeTest", "from . import SyncSmokeTest, UNLOADABLE_MAPS")
    t = replace(t, "and m != '/Game/Carla/Maps/Town12/Town12':",
                "and m != '/Game/Carla/Maps/Town12/Town12' \\\n"
                "                    and m.split('/')[-1] not in UNLOADABLE_MAPS:")
    t = replace(t, "self.assertFalse(any(x.error for x in response))",
                "self.assertFalse(any(x.error for x in response), \"%s on %s: %s\" % (  # port: say which\n"
                "                        vehicle.id, m, sorted({(t.location.x, t.location.y, x.error)\n"
                "                                               for x, t in zip(response, spawn_points) if x.error})))")
    return t, ["skips the maps in UNLOADABLE_MAPS (RoadgenCross), as test_map does",
               "the spawn-error assertion names the blueprint, the map and each failing spawn "
               "point with its error (upstream's only says `True is not false`)",
               "otherwise copied so that `from . import SyncSmokeTest` uses the ported base"]


def _sync(t: str):
    t = replace(t, "bp_lib.find('vehicle.ford.mustang')", "bp_lib.find('vehicle.ue4.ford.mustang')")
    return t, ["blueprint vehicle.ford.mustang is vehicle.ue4.ford.mustang on ue5-dev (renamed; "
               "the old id is not in the library)",
               "otherwise copied so that `from . import SmokeTest` uses the ported base"]


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
    t = replace(t, "carla.libcarla.Vector3D", "carla.Vector3D")
    # Town05_Opt: a straight road along +y at x = 31, from y = -200.
    t = replace(t, '''        self.client.load_world("Town05_Opt", False)
        # workaround: give time to UE4 to clean memory after loading (old assets)
        time.sleep(5)
''', '''        self.client.load_world(shipped_map(self.client, LARGE_MAPS), False)
        # workaround: give time to UE4 to clean memory after loading (old assets)
        time.sleep(5)
        self.world = self.client.get_world()
        self.frame = MapFrame(self.world, (31, -200), 90.0, 250.0, side="left")
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
        "straight road of the loaded map that has a same-direction lane beside it (the "
        "second vehicle drives 4-5 m to the side); readings of .y go back through it, so the "
        "assertions are unchanged",
        "`carla.libcarla.Vector3D` (UE4 package layout, absent from ue5-dev's official module "
        "too) is `carla.Vector3D`"]


def _api_collision(t: str):
    t = replace(t, "world = client.load_world('Town01')",
                "client.set_timeout(60.0)  # port: Town10HD_Opt takes ~8 s to load\n"
                "        world = client.load_world(shipped_map(client))\n"
                "        frame = MapFrame(world, (177.7, 198.8), 0.0, 40.0)")
    t = replace(t, 'bp_lib.filter("*mkz_2020*")', 'bp_lib.filter("vehicle.lincoln.mkz")')
    t = replace(t, "self._on_collision(self. event)", "self._on_collision(event)")
    t = re.sub(r"walker = world\.spawn_actor\(bp, carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(\)\)\)",
               r"walker = frame.spawn(world, bp, frame.transform(\1))", t)
    t = re.sub(r"vehicle = world\.spawn_actor\(bp, carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(\)\)\)",
               r"vehicle = frame.spawn(world, bp, frame.transform(\1), move=False)", t)
    t = replace(t, "import carla\n", "import carla\n" + HELPERS, 1)
    return t, ["loads a shipped map instead of Town01, with a 60 s client timeout (the default "
               "5 s was enough for Town01 on UE4; Town10HD_Opt takes ~8 s)",
               "the walker and the vehicle behind it, on Town01's road at y = 199 heading +x, go "
               "through a MapFrame onto a straight road of the loaded map",
               "blueprint *mkz_2020* (UE4) is vehicle.lincoln.mkz on ue5-dev (stale upstream id; "
               "Docs/catalogue_vehicles.md at the same commit)",
               "upstream bug: the collision callback passes `self. event` (self.event, an "
               "AttributeError raised inside the callback), so no collision could ever be counted; "
               "it passes `event`",
               "they spawn through MapFrame.spawn: a blocked spawn (UE4's flat-ground z) is "
               "retried 1 m higher, and the walker's further along the lane, the frame moving "
               "with it so the vehicle still starts 23 m behind it"]


def _top_vehicle_physics(t: str):
    t = replace(t, '''        if world.get_map().name != "Town05":
            client.load_world("Town05", False)
''', "")
    # Load the map before switching to synchronous mode: loading in it waits
    # for ticks that never come.
    t = replace(t, '''        # Setting the world and the spawn properties
        original_settings = world.get_settings()''', '''        if world.get_map().name.split("/")[-1] != shipped_map(client, LARGE_MAPS):
            client.load_world(shipped_map(client, LARGE_MAPS), False)
        world = client.get_world()
        global FRAME, BASE_FRAME
        FRAME = BASE_FRAME = MapFrame(world, (32, -180), 90.0, 250.0)
        # Setting the world and the spawn properties
        original_settings = world.get_settings()''')
    # Spawn points and stop conditions in Town05 coordinates.
    t = re.sub(r"(init_(?:loc|pos) = )carla\.Transform\(carla\.Location\(([^()]*)\), carla\.Rotation\(([^()]*)\)\)",
               r"\1scenario_start(world, \2, \3)", t)
    t = replace(t, "    vehicle = world.spawn_actor(bp_veh, veh_transf)\n",
                "    vehicle = FRAME.spawn(world, bp_veh, veh_transf)\n")
    t = replace(t, "        loc = vehicle.get_location()\n", "        loc = FRAME.up_location(vehicle.get_location())\n")
    t = replace(t, "        rot = vehicle.get_transform().rotation\n",
                "        rot = vehicle.get_transform().rotation\n        rot.yaw = FRAME.up_yaw(rot.yaw)\n")
    t = replace(t, "import carla\n", "import carla\n" + HELPERS + '''
FRAME = BASE_FRAME = None


def scenario_start(world, x, y, z=0.0, yaw=0.0, pitch=0.0, roll=0.0):
    """port: a scenario's Town05 start as a transform. The scenarios start at
    different places of Town05 (the u-turn 17 m beside the road), so each one
    gets FRAME anchored at its own start, on the lane's start, heading along it;
    its stop conditions read locations through FRAME."""
    global FRAME
    FRAME = BASE_FRAME.at(world, (x, y), yaw)
    return FRAME.transform(x, y, z, yaw=yaw, pitch=pitch, roll=roll)
''', 1)
    return t, ["loads a shipped large map (Town15, else Mine_01 or Town10HD_Opt) instead of Town05, "
               "before switching to synchronous mode rather than after (loading in synchronous "
               "mode waits for ticks that never come)",
               "the scenarios' start transforms and the stop conditions on location and yaw, "
               "written for Town05, go through a MapFrame anchored at each scenario's own start "
               "(scenario_start), so every scenario starts on the lane heading along it (the "
               "u-turn and high-speed-turn starts lie off Town05's road at x = 32)",
               "run_scenario spawns through MapFrame.spawn: a blocked spawn is retried 1 m higher, "
               "then further along the lane"]


# Smoke tests that fail only through SmokeTest.tearDown (Town03).
BASE_ONLY = ["test_blueprint", "test_client", "test_collision_sensor", "test_geoconversion",
             "test_lidar", "test_props_loading", "test_sensor_determinism",
             "test_sensor_tick_time", "test_snapshot", "test_streamming",
             "test_world"]

RULES = {
    "smoke/__init__.py": _smoke_init,
    **{f"smoke/{n}.py": _copy for n in BASE_ONLY},
    "smoke/test_map.py": _map,
    "smoke/test_sync.py": _sync,
    "smoke/test_spawnpoints.py": _spawnpoints,
    "smoke/test_determinism.py": _determinism,
    "smoke/test_collision_determinism.py": _collision_determinism,
    "smoke/test_vehicle_physics.py": _vehicle_physics,
    "API/test_collision.py": _api_collision,
    "test_vehicle_physics.py": _top_vehicle_physics,
}


# The map each original needs (for its `exclude:` entry).
NEEDED_MAP = {
    **{f"smoke/{n}.py": "Town03" for n in BASE_ONLY + ["test_map", "test_sync", "test_spawnpoints"]},
    "smoke/test_determinism.py": "Town03",
    "smoke/test_collision_determinism.py": "Town03",
    "smoke/test_vehicle_physics.py": "Town05_Opt",
    "API/test_collision.py": "Town01",
    "test_vehicle_physics.py": "Town05",
}


# Ported tests that also call reload_world, which fails on the ue5-dev server
# build (OpenDRIVE parse error; the official module too): a map cannot fix
# them, so they stay excluded.
NEEDS_RELOAD = {
    "smoke/test_map.py": ["TestMap.test_reload_world"],
    "smoke/test_snapshot.py": ["TestSnapshot.test_spawn_points"],
    "smoke/test_sync.py": ["TestSynchronousMode.test_reloading_map"],
    "smoke/test_sensor_determinism.py": ["TestSensorDeterminism.test_all_sensors"],
    "smoke/test_determinism.py": ["TestDeterminism.test_determ"],
    "smoke/test_collision_determinism.py": [
        "TestCollisionDeterminism.test_two_cars", "TestCollisionDeterminism.test_three_cars",
        "TestCollisionDeterminism.test_car_bike", "TestCollisionDeterminism.test_car_walker"],
}


# Ported tests that fail with the official module on ue5-dev for reasons a
# map cannot fix (content or server limits; checked with --mode official).
CONTENT_LIMITS = {
    "smoke/test_blueprint.py": {"TestBlueprintLibrary.test_blueprint_ids":
        "ue5-dev ships blueprint id blueprint.trafficlightexample, which has two dot-separated "
        "parts, not the three the test expects (the official module fails the same way)"},
    "smoke/test_props_loading.py": {"TestPropsLoading.test_spawn_loaded_props":
        "a static prop fails to spawn on ue5-dev ('Unknown error'; the official module fails "
        "the same way)"},
    "smoke/test_sensor_tick_time.py": {"TestSensorTickTime.test_sensor_tick_time":
        "sensors with sensor_tick deliver no data on this server (the official module fails "
        "the same way)"},
    "smoke/test_streamming.py": {"TestStreamming.test_multistream":
        "extra clients with wait_for_tick intermittently hang on this server (the official "
        "module timed out; a typesafe run passed)"},
}


CONTENT_LIMITS["smoke/test_vehicle_physics.py"] = {
    "TestVehicleFriction.test_vehicle_zero_friction":
        "physics on this ue5-dev server: the official module fails it too, with "
        "vehicle.taxi.ford's velocities after initialisation [27.599, 27.764] against the "
        "27.778 reference",
    "TestVehicleTireConfig.test_vehicle_wheel_collision":
        "physics on this ue5-dev server: the official module fails it too, with "
        "vehicle.taxi.ford's two velocities after the simulation unequal, [-0.807, -2.364]",
    "TestVehicleTireConfig.test_vehicle_tire_long_stiff":
        "physics on this ue5-dev server: the official module fails it too; two identical "
        "vehicle.firetruck.actors side by side at full throttle drive 29.73 m and 24.11 m "
        "(the test needs the second to go at least as far)",
}


# Ported tests the official module cannot run because of its own defects
# (expectations.yaml `official:`): official runs leave them out, typesafe
# runs keep them (typesafe_carla is just not compared with it there).
OFFICIAL_DEFECTS = {
    "smoke/test_vehicle_physics.py": {
        t: "the official module's bindings fail in get_physics_control(): TypeError: No "
           "to_python (by-value) converter found for C++ type: std::vector<float>"
        for t in ("TestApplyVehiclePhysics.test_single_physics_control",
                  "TestApplyVehiclePhysics.test_multiple_physics_control")},
}


def exclude_originals(refs=("ue5-dev",)) -> None:
    """Marks the ported originals `exclude:` in tests/upstream/expectations.yaml,
    and the ported tests that need reload_world."""
    from tools import upstream_tests as up

    for mode in up.MODES:
        manifest = up.load_manifest(mode)
        for ref in refs:
            entries = manifest.setdefault(ref, {})
            for rel, name in NEEDED_MAP.items():
                entries[rel] = (f"exclude: map {name} not shipped in ue5-dev packages; "
                                f"ported to tests/upstream/ported/{rel}")
            for rel, tests in NEEDS_RELOAD.items():
                ported = entries.get(f"ported/{rel}")
                ported = ported if isinstance(ported, dict) else {}
                for tid in tests:
                    ported[tid] = ("exclude: needs reload_world, which fails on the ue5-dev server "
                                   "build (OpenDRIVE parse error; the official module too)")
                entries[f"ported/{rel}"] = ported
            for rel, tests in CONTENT_LIMITS.items():
                ported = entries.get(f"ported/{rel}")
                ported = ported if isinstance(ported, dict) else {}
                for tid, why in tests.items():
                    ported[tid] = f"exclude: {why}"
                entries[f"ported/{rel}"] = ported
        up.write_manifest(mode, manifest)
    official = up.load_manifest("official")
    for ref in refs:
        official[ref] = {f"ported/{rel}": {t: f"exclude: {why}" for t, why in tests.items()}
                         for rel, tests in OFFICIAL_DEFECTS.items()}
    up.write_manifest("official", official)


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
