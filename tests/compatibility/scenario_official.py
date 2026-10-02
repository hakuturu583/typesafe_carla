"""The compatibility scenario using the official CARLA Python API.

Prints one `key=value` line per observation; scenario_typesafe.codon prints
the same keys. compare.py runs both against one server and compares them.
"""

import os
import sys

import carla

host = os.environ.get("TSC_CARLA_HOST", "localhost")
port = int(os.environ.get("TSC_CARLA_PORT", "2000"))


def out(key, value):
    print(f"{key}={value}")


client = carla.Client(host, port)
client.set_timeout(20.0)
out("server_version", client.get_server_version())
world = client.get_world()
lib = world.get_blueprint_library()
out("vehicle_blueprints", ",".join(sorted(bp.id for bp in lib.filter("vehicle.*"))))
bp = lib.find("vehicle.lincoln.mkz")
out("color_type", str(bp.get_attribute("color").type))
out("wheels", bp.get_attribute("number_of_wheels").as_int())

# Milestone 1: map, waypoints, batch errors.
m = world.get_map()
out("map_name", m.name)
points = m.get_spawn_points()
out("spawn_points", len(points))
p0 = points[0]
out("spawn0", f"{p0.location.x:.3f},{p0.location.y:.3f},{p0.location.z:.3f},{p0.rotation.yaw:.3f}")
wp = m.get_waypoint(p0.location)
out("waypoint", f"{wp.road_id},{wp.section_id},{wp.lane_id},{int(wp.lane_type)}")
out("waypoint_s", f"{wp.s:.3f},{wp.lane_width:.3f}")
n = wp.next(10.0)[0].transform.location
out("next10", f"{n.x:.3f},{n.y:.3f}")
out("generated", len(m.generate_waypoints(20.0)))
r = client.apply_batch_sync([carla.command.DestroyActor(999999)])
out("batch_error", r[0].error)

spawn = carla.Transform(carla.Location(-64.644844, 24.471010, 0.6), carla.Rotation(0.0, 0.159198, 0.0))
original = world.get_settings()
vehicle = world.spawn_actor(bp, spawn)
try:
    settings = world.get_settings()
    settings.synchronous_mode = True
    settings.fixed_delta_seconds = 0.05
    world.apply_settings(settings)
    out("type_id", vehicle.type_id)
    e = vehicle.bounding_box.extent
    out("bbox", f"{e.x:.3f},{e.y:.3f},{e.z:.3f}")
    pc = vehicle.get_physics_control()
    out("physics", f"{pc.mass:.3f},{pc.max_rpm:.3f},{pc.wheels[0].wheel_radius:.3f}")
    out("physics_wheels", len(pc.wheels))
    for _ in range(20):
        world.tick()
    t = vehicle.get_transform()
    out("settled", f"{t.location.x:.3f},{t.location.y:.3f},{t.rotation.yaw:.3f}")
    vehicle.apply_control(carla.VehicleControl(throttle=0.6, steer=0.0))
    for _ in range(40):
        world.tick()
    c = vehicle.get_control()
    out("control", f"{c.throttle:.3f},{c.steer:.3f},{c.brake:.3f},{int(c.reverse)},{c.gear}")
    loc = vehicle.get_location()
    out("driven", f"{loc.x:.3f},{loc.y:.3f}")
    out("speed", f"{vehicle.get_velocity().length():.3f}")
finally:
    world.apply_settings(original)
    out("destroyed", int(vehicle.destroy()))
sys.stdout.flush()
