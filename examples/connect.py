# Milestone 0 success criterion: connect, find a vehicle, drive it.
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
client.set_timeout(10.0)
print("server version:", client.get_server_version())

world = client.get_world()
vehicle = world.get_actors()[0].as_vehicle()
vehicle.apply_control(carla.VehicleControl(throttle=0.2, steer=0.0))
print("driving", vehicle, "at", vehicle.get_transform())
