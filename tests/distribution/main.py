# A downstream application, as a user would write it after
# `uv add typesafe-carla`. Used by clean_env.sh (design section 44).
import os
import typesafe_carla as carla

client = carla.Client(os.getenv("TSC_CARLA_HOST", "localhost"), int(os.getenv("TSC_CARLA_PORT", "2000")))
client.set_timeout(20.0)
world = client.get_world()
carla_map = world.get_map()
print("backend", carla.backend(), "libcarla", carla.libcarla_version(), "server",
      client.get_server_version(), "map", carla_map.name)

lib = world.get_blueprint_library()
point = carla_map.get_spawn_points()[3]
vehicle = world.spawn_actor(lib.find("vehicle.lincoln.mkz"), point).as_vehicle()
camera = world.spawn_actor(lib.find("sensor.camera.rgb"), carla.Transform(carla.Location(1.5, 0.0, 2.0)),
                           attach_to=vehicle).as_sensor()
original = world.get_settings()
settings = world.get_settings()
settings.synchronous_mode = True
settings.fixed_delta_seconds = 0.05
world.apply_settings(settings)
try:
    camera.listen(2)
    vehicle.apply_control(carla.VehicleControl(throttle=0.7))
    start = vehicle.get_location()
    for _ in range(60):
        world.tick()
    image = camera.wait_for_data(10.0).as_image()
    moved = vehicle.get_location().distance(start)
    print("drove", moved, "m; last image", image.width, "x", image.height, "frame", image.frame)
    assert moved > 2.0
finally:
    camera.stop()
    world.apply_settings(original)
    camera.destroy()
    vehicle.destroy()
print("DOWNSTREAM OK")
