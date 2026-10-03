# Ported from CARLA's PythonAPI/test/API/test_collision.py at carla-simulator/carla@0a5ce0d5b4952bd8294a163c12d49f197bdb2aba
# by tools/port_upstream_tests.py (typesafe_carla issue #80): the original needs a map
# (Town03 / Town05 / Town01) that no ue5-dev package ships. Do not edit by hand:
# change the rules there and regenerate. Changes:
#   - loads a shipped map instead of Town01
#   - the walker and the vehicle behind it, on Town01's road at y = 199 heading +x, go through a MapFrame onto a straight road of the loaded map
#

from __future__ import print_function

import unittest
import argparse
import glob
import math
import os
import sys
import time
try:
    sys.path.append(glob.glob('../carla/dist/carla-*%d.%d-%s.egg' % (
        sys.version_info.major,
        sys.version_info.minor,
        'win-amd64' if os.name == 'nt' else 'linux-x86_64'))[0])
except IndexError:
    pass
try:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/carla')
except IndexError:
    pass

import carla

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


#record the result as json to the same folder


class TestCollision(unittest.TestCase):
    def setUp(self):
        self.collisions = 0

    def _on_collision(self, event ):
        self.collisions += 1

    def test_collision_against_side_of_car(self):
        client = carla.Client()
        world = client.load_world(shipped_map(client))
        frame = MapFrame(world, (177.7, 198.8), 0.0, 40.0)
        bp_lib = world.get_blueprint_library()
        spectator = world.get_spectator()

        # Spawn the actor
        bp = bp_lib.filter("*walker*")[0]
        # bp.set_attribute('is_invincible', 'false')
        walker = world.spawn_actor(bp, frame.transform(200.7, 199.3, 0.2))

        bp = bp_lib.filter("*mkz_2020*")[0]
        vehicle = world.spawn_actor(bp, frame.transform(177.7, 198.8, 0.2))
        spectator.set_transform(carla.Transform(carla.Location(205.9, 193.2, 3.9), carla.Rotation(pitch=-29, yaw=135)))

        collision_bp = bp_lib.find('sensor.other.collision')
        sensor_collision = world.spawn_actor(collision_bp, carla.Transform(), attach_to=vehicle)
        sensor_collision.listen(lambda event: self._on_collision(self. event))

        vehicle.apply_control(carla.VehicleControl(throttle=1))

        for _ in range(400):
            world.tick()

        walker.destroy()
        sensor_collision.destroy()
        vehicle.destroy()

        self.assertNotEqual(self.collisions, 0)

