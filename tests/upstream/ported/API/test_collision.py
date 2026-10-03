# Ported from CARLA's PythonAPI/test/API/test_collision.py at carla-simulator/carla@0a5ce0d5b4952bd8294a163c12d49f197bdb2aba
# by tools/port_upstream_tests.py (typesafe_carla issue #80): the original needs a map
# (Town03 / Town05 / Town01) that no ue5-dev package ships. Do not edit by hand:
# change the rules there and regenerate. Changes:
#   - loads a shipped map instead of Town01, with a 60 s client timeout (the default 5 s was enough for Town01 on UE4; Town10HD_Opt takes ~8 s)
#   - the walker and the vehicle behind it, on Town01's road at y = 199 heading +x, go through a MapFrame onto a straight road of the loaded map
#   - blueprint *mkz_2020* (UE4) is vehicle.lincoln.mkz on ue5-dev (stale upstream id; Docs/catalogue_vehicles.md at the same commit)
#   - upstream bug: the collision callback passes `self. event` (self.event, an AttributeError raised inside the callback), so no collision could ever be counted; it passes `event`
#   - they spawn through MapFrame.spawn: a blocked spawn (UE4's flat-ground z) is retried 1 m higher, and the walker's further along the lane, the frame moving with it so the vehicle still starts 23 m behind it
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


#record the result as json to the same folder


class TestCollision(unittest.TestCase):
    def setUp(self):
        self.collisions = 0

    def _on_collision(self, event ):
        self.collisions += 1

    def test_collision_against_side_of_car(self):
        client = carla.Client()
        client.set_timeout(60.0)  # port: Town10HD_Opt takes ~8 s to load
        world = client.load_world(shipped_map(client))
        frame = MapFrame(world, (177.7, 198.8), 0.0, 40.0)
        bp_lib = world.get_blueprint_library()
        spectator = world.get_spectator()

        # Spawn the actor
        bp = bp_lib.filter("*walker*")[0]
        # bp.set_attribute('is_invincible', 'false')
        walker = frame.spawn(world, bp, frame.transform(200.7, 199.3, 0.2))

        bp = bp_lib.filter("vehicle.lincoln.mkz")[0]
        vehicle = frame.spawn(world, bp, frame.transform(177.7, 198.8, 0.2), move=False)
        spectator.set_transform(carla.Transform(carla.Location(205.9, 193.2, 3.9), carla.Rotation(pitch=-29, yaw=135)))

        collision_bp = bp_lib.find('sensor.other.collision')
        sensor_collision = world.spawn_actor(collision_bp, carla.Transform(), attach_to=vehicle)
        sensor_collision.listen(lambda event: self._on_collision(event))

        vehicle.apply_control(carla.VehicleControl(throttle=1))

        for _ in range(400):
            world.tick()

        walker.destroy()
        sensor_collision.destroy()
        vehicle.destroy()

        self.assertNotEqual(self.collisions, 0)

