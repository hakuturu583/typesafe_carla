# Deterministic fixed-step simulation.
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
client.set_timeout(10.0)
world = client.get_world()

original = world.get_settings()
settings = world.get_settings()
settings.synchronous_mode = True
settings.fixed_delta_seconds = 0.05
world.apply_settings(settings)

try:
    bp = world.get_blueprint_library().find("vehicle.audi.tt")
    vehicle = world.spawn_actor(bp, carla.Transform(carla.Location(0.0, 30.0, 0.5))).as_vehicle()
    vehicle.apply_control(carla.VehicleControl(throttle=0.6))
    for _ in range(40):
        frame = world.tick()
    print("frame", frame, "location", vehicle.get_location(),
          "speed", vehicle.get_velocity().length())
    vehicle.destroy()
finally:
    world.apply_settings(original)
