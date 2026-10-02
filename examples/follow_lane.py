# Drive a vehicle along its lane with a pure-pursuit controller on waypoints,
# in synchronous mode. Spawns its own vehicle at the map's first spawn point.
import math
import typesafe_carla as carla

def steer_towards(vehicle: carla.Vehicle, target: carla.Location) -> float:
    t = vehicle.get_transform()
    yaw = math.radians(t.rotation.yaw)
    dx = target.x - t.location.x
    dy = target.y - t.location.y
    angle = math.atan2(dy, dx) - yaw
    angle = math.atan2(math.sin(angle), math.cos(angle))  # wrap to [-pi, pi]
    return max(-1.0, min(1.0, angle / math.radians(45.0)))

client = carla.Client("localhost", 2000)
client.set_timeout(20.0)
world = client.get_world()
m = world.get_map()
bp = world.get_blueprint_library().filter("vehicle.*")[0]
vehicle = world.spawn_actor(bp, m.get_spawn_points()[0]).as_vehicle()

original = world.get_settings()
settings = world.get_settings()
settings.synchronous_mode = True
settings.fixed_delta_seconds = 0.05
world.apply_settings(settings)
try:
    for step in range(200):
        wp = m.get_waypoint(vehicle.get_location())
        if wp is None:
            break
        ahead = wp.next(6.0)
        if not ahead:
            break
        steer = steer_towards(vehicle, ahead[0].transform.location)
        speed = vehicle.get_velocity().length()
        vehicle.apply_control(carla.VehicleControl(throttle=0.5 if speed < 8.0 else 0.0,
                                                   steer=steer))
        frame = world.tick()
        if step % 50 == 0:
            print("frame", frame, "road", wp.road_id, "lane", wp.lane_id, "speed", speed)
finally:
    world.apply_settings(original)
    vehicle.destroy()
