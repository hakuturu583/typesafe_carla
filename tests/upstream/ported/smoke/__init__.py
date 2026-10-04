# Ported from CARLA's PythonAPI/test/smoke/__init__.py at carla-simulator/carla@0a5ce0d5b4952bd8294a163c12d49f197bdb2aba
# by tools/port_upstream_tests.py (typesafe_carla issue #80): the original needs a map
# (Town03 / Town05 / Town01) that no ue5-dev package ships. Do not edit by hand:
# change the rules there and regenerate. Changes:
#   - tearDown loads a shipped map (Town10HD_Opt) instead of Town03
#   - the large-vehicle exclusion list (UE4 ids only, so stale on ue5-dev) also names ue5-dev's trucks and buses (base_type truck / bus in Docs/catalogue_vehicles.md at the same commit): vehicle.carlacola.actors, vehicle.firetruck.actors, vehicle.fuso.mitsubishi and vehicle.miningtruck.miningtruck
#   - adds shipped_map() and MapFrame for the ported tests
#
# Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma de
# Barcelona (UAB).
#
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT>.

import glob
import os
import sys
import unittest

try:
    sys.path.append(glob.glob('../carla/dist/carla-*%d.%d-%s.egg' % (
        sys.version_info.major,
        sys.version_info.minor,
        'win-amd64' if os.name == 'nt' else 'linux-x86_64'))[0])
except IndexError:
    pass

import carla
import time

TESTING_ADDRESS = ('localhost', 3654)
VEHICLE_VEHICLES_EXCLUDE_FROM_OLD_TOWNS = ['vehicle.mitsubishi.fusorosa', 'vehicle.carlamotors.european_hgv', 'vehicle.carlamotors.firetruck',
    # port: ue5-dev's trucks and buses (Docs/catalogue_vehicles.md base types)
    'vehicle.carlacola.actors', 'vehicle.firetruck.actors', 'vehicle.fuso.mitsubishi',
    'vehicle.miningtruck.miningtruck']

class SmokeTest(unittest.TestCase):
    def setUp(self):
        self.testing_address = TESTING_ADDRESS
        self.client = carla.Client(*TESTING_ADDRESS)
        self.vehicle_vehicles_exclude_from_old_towns = VEHICLE_VEHICLES_EXCLUDE_FROM_OLD_TOWNS
        self.client.set_timeout(120.0)
        self.world = self.client.get_world()

    def tearDown(self):
        self.client.load_world(shipped_map(self.client))
        # workaround: give time to UE4 to clean memory after loading (old assets)
        time.sleep(5)
        self.world = None
        self.client = None
    
    def filter_vehicles_for_old_towns(self, blueprint_list):
        new_list = []
        for blueprint in blueprint_list:
            if blueprint.id not in self.vehicle_vehicles_exclude_from_old_towns:
                new_list.append(blueprint)
        return new_list


class SyncSmokeTest(SmokeTest):
    def setUp(self):
        super(SyncSmokeTest, self).setUp()
        self.settings = self.world.get_settings()
        settings = carla.WorldSettings(
            no_rendering_mode=False,
            synchronous_mode=True,
            fixed_delta_seconds=0.05)
        self.world.apply_settings(settings)
        self.world.tick()

    def tearDown(self):
        self.world.apply_settings(self.settings)
        if self.settings.synchronous_mode:
            # only tick when the restored settings keep synchronous mode active
            self.world.tick()
        self.settings = None
        super(SyncSmokeTest, self).tearDown()

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
