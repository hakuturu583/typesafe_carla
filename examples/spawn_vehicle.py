# Spawn a vehicle from a blueprint, then remove it again.
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
client.set_timeout(10.0)
world = client.get_world()

blueprints = world.get_blueprint_library()
bp = blueprints.find("vehicle.tesla.model3")
if bp.has_attribute("color"):
    bp.set_attribute("color", "255,0,0")

spawn = carla.Transform(carla.Location(10.0, 20.0, 0.5), carla.Rotation(0.0, 90.0, 0.0))
actor = world.try_spawn_actor(bp, spawn)
if actor is None:
    print("spawn point occupied")
else:
    vehicle = actor.as_vehicle()
    print("spawned", vehicle, "at", vehicle.get_transform())
    vehicle.destroy()
