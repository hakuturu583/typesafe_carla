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

spawn = carla.Transform(carla.Location(-64.644844, 24.471010, 0.6), carla.Rotation(0.0, 0.159198, 0.0))
original = world.get_settings()
vehicle = world.spawn_actor(bp, spawn)
try:
    settings = world.get_settings()
    settings.synchronous_mode = True
    settings.fixed_delta_seconds = 0.05
    world.apply_settings(settings)
    out("type_id", vehicle.type_id)
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
